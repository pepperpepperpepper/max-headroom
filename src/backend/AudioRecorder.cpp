#include "AudioRecorder.h"

#include "PipeWireThread.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <sndfile.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMinDb = -100.0f;

const char* stateToString(pw_stream_state state)
{
  switch (state) {
    case PW_STREAM_STATE_ERROR:
      return "error";
    case PW_STREAM_STATE_UNCONNECTED:
      return "unconnected";
    case PW_STREAM_STATE_CONNECTING:
      return "connecting";
    case PW_STREAM_STATE_PAUSED:
      return "paused";
    case PW_STREAM_STATE_STREAMING:
      return "streaming";
  }
  return "unknown";
}

float amplitudeToDb(float amp)
{
  if (!std::isfinite(amp) || amp <= 0.0f) {
    return kMinDb;
  }
  const float db = 20.0f * std::log10(amp);
  if (!std::isfinite(db)) {
    return kMinDb;
  }
  return std::max(kMinDb, db);
}

QString expandHomeDir(QString path)
{
  if (path == QStringLiteral("~")) {
    return QDir::homePath();
  }
  if (path.startsWith(QStringLiteral("~/"))) {
    return QDir::home().filePath(path.mid(2));
  }
  return path;
}

QString sanitizePathComponent(QString s)
{
  s = s.trimmed();
  if (s.isEmpty()) {
    return QStringLiteral("recording");
  }
  for (QChar& c : s) {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')) {
      c = QLatin1Char('_');
    }
  }
  return s;
}

} // namespace

AudioRecorder::AudioRecorder(PipeWireThread* pw, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
{
}

AudioRecorder::~AudioRecorder()
{
  stop();
}

QString AudioRecorder::formatToString(Format format)
{
  switch (format) {
    case Format::Wav:
      return QStringLiteral("wav");
    case Format::Flac:
      return QStringLiteral("flac");
  }
  return QStringLiteral("wav");
}

std::optional<AudioRecorder::Format> AudioRecorder::formatFromString(const QString& s)
{
  const QString v = s.trimmed().toLower();
  if (v == QStringLiteral("wav") || v == QStringLiteral("wave")) {
    return Format::Wav;
  }
  if (v == QStringLiteral("flac")) {
    return Format::Flac;
  }
  return std::nullopt;
}

QString AudioRecorder::defaultExtensionForFormat(Format format)
{
  return formatToString(format);
}

QString AudioRecorder::ensureFileExtension(QString path, Format format)
{
  path = expandHomeDir(path.trimmed());
  if (path.isEmpty()) {
    return path;
  }

  const QString wanted = defaultExtensionForFormat(format).toLower();
  const QFileInfo fi(path);
  const QString suffix = fi.suffix();
  if (suffix.isEmpty()) {
    return path + QStringLiteral(".") + wanted;
  }

  const QString suffixLower = suffix.toLower();
  if ((suffixLower == QStringLiteral("wav") || suffixLower == QStringLiteral("flac")) && suffixLower != wanted) {
    QString out = path;
    out.chop(suffix.size());
    return out + wanted;
  }

  return path;
}

QString AudioRecorder::expandPathTemplate(const QString& templateOrPath, const QString& targetLabel, Format format)
{
  QString out = expandHomeDir(templateOrPath.trimmed());
  if (out.isEmpty()) {
    return out;
  }

  const QDateTime now = QDateTime::currentDateTime();
  const QString date = now.toString(QStringLiteral("yyyyMMdd"));
  const QString time = now.toString(QStringLiteral("HHmmss"));
  const QString datetime = now.toString(QStringLiteral("yyyyMMdd-HHmmss"));

  const QString target = sanitizePathComponent(targetLabel);

  out.replace(QStringLiteral("{date}"), date);
  out.replace(QStringLiteral("{time}"), time);
  out.replace(QStringLiteral("{datetime}"), datetime);
  out.replace(QStringLiteral("{ts}"), datetime);
  out.replace(QStringLiteral("{target}"), target);
  out.replace(QStringLiteral("{format}"), formatToString(format));
  out.replace(QStringLiteral("{ext}"), defaultExtensionForFormat(format));

  return ensureFileExtension(out, format);
}

QString AudioRecorder::lastError() const
{
  std::lock_guard<std::mutex> lock(m_errorMutex);
  return m_lastError;
}

pw_stream_state AudioRecorder::streamState() const
{
  return static_cast<pw_stream_state>(m_streamState.load());
}

QString AudioRecorder::streamStateString() const
{
  return QString::fromUtf8(stateToString(streamState()));
}

bool AudioRecorder::start(const StartOptions& options)
{
  if (options.filePath.trimmed().isEmpty()) {
    setErrorAndScheduleStop(tr("Invalid output path."));
    return false;
  }

  stop();

  m_stopScheduled.store(false);
  m_streamState.store(PW_STREAM_STATE_UNCONNECTED);
  m_dataBytesWritten.store(0);
  m_framesCaptured.store(0);
  m_peakDb.store(kMinDb);
  m_rmsDb.store(kMinDb);
  m_sampleRate.store(48000);
  m_channels.store(2);
  m_quantumFrames.store(0);
  m_lastBufferFrames.store(0);
  m_lastBufferBytes.store(0);
  m_captureSink.store(options.captureSink);
  m_format.store(static_cast<int>(options.format));

  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();
  }

  m_targetObject = options.targetObject.trimmed();

  const QString label =
      !m_targetObject.isEmpty() ? m_targetObject : (options.captureSink ? QStringLiteral("system-mix") : QStringLiteral("default-input"));
  const QString resolved = expandPathTemplate(options.filePath, label, options.format);
  m_filePath = resolved;

  const QFileInfo fi(resolved);
  if (!fi.absolutePath().isEmpty()) {
    QDir dir;
    if (!dir.mkpath(fi.absolutePath())) {
      setErrorAndScheduleStop(tr("Failed to create output directory."));
      return false;
    }
  }

  SF_INFO info{};
  info.samplerate = static_cast<int>(sampleRate());
  info.channels = static_cast<int>(channels());
  info.format = 0;

  switch (options.format) {
    case Format::Wav:
      info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
      break;
    case Format::Flac:
      info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
      break;
  }

  const QByteArray pathLocal = resolved.toLocal8Bit();
  m_sndFile = sf_open(pathLocal.constData(), SFM_WRITE, &info);
  if (!m_sndFile) {
    setErrorAndScheduleStop(tr("Failed to open output file: %1").arg(QString::fromLocal8Bit(sf_strerror(nullptr))));
    return false;
  }

  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    setErrorAndScheduleStop(tr("Not connected to PipeWire."));
    sf_close(m_sndFile);
    m_sndFile = nullptr;
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  connectStreamLocked();
  pw_thread_loop_unlock(loop);

  if (!m_stream) {
    setErrorAndScheduleStop(tr("Failed to connect recording stream."));
    sf_close(m_sndFile);
    m_sndFile = nullptr;
    return false;
  }

  emit recordingChanged(true);
  return true;
}

bool AudioRecorder::startWav(const QString& filePath, const QString& targetObject, bool captureSink)
{
  StartOptions o;
  o.filePath = filePath;
  o.targetObject = targetObject;
  o.captureSink = captureSink;
  o.format = Format::Wav;
  return start(o);
}

void AudioRecorder::stop()
{
  const bool wasRecording = (m_stream != nullptr);

  if (m_pw && m_pw->threadLoop()) {
    pw_thread_loop* loop = m_pw->threadLoop();
    pw_thread_loop_lock(loop);
    destroyStreamLocked();
    pw_thread_loop_unlock(loop);
  } else {
    destroyStreamLocked();
  }

  if (m_sndFile) {
    sf_write_sync(m_sndFile);
    sf_close(m_sndFile);
    m_sndFile = nullptr;
  }

  if (wasRecording) {
    emit recordingChanged(false);
  }
}

void AudioRecorder::onStreamStateChanged(void* data, enum pw_stream_state /*old*/, enum pw_stream_state state, const char* error)
{
  auto* self = static_cast<AudioRecorder*>(data);
  if (!self) {
    return;
  }

  self->m_streamState.store(state);

  if (state == PW_STREAM_STATE_ERROR) {
    const QString msg = error
        ? QStringLiteral("%1: %2").arg(QString::fromUtf8(stateToString(state)), QString::fromUtf8(error))
        : QString::fromUtf8(stateToString(state));
    self->setErrorAndScheduleStop(msg);
  }
}

void AudioRecorder::onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
  auto* self = static_cast<AudioRecorder*>(data);
  if (!self || !param) {
    return;
  }
  if (id != SPA_PARAM_Format) {
    return;
  }

  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0) {
    return;
  }

  if (info.rate > 0) {
    self->m_sampleRate.store(info.rate);
  }
  if (info.channels > 0) {
    self->m_channels.store(info.channels);
  }
}

void AudioRecorder::onStreamProcess(void* data)
{
  auto* self = static_cast<AudioRecorder*>(data);
  if (!self || !self->m_stream) {
    return;
  }

  pw_buffer* buffer = pw_stream_dequeue_buffer(self->m_stream);
  if (!buffer) {
    return;
  }
  self->processBuffer(buffer);
  pw_stream_queue_buffer(self->m_stream, buffer);
}

void AudioRecorder::connectStreamLocked()
{
  if (m_stream || !m_pw || !m_pw->core()) {
    return;
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Production",
      PW_KEY_NODE_NAME, "headroom.recorder",
      PW_KEY_STREAM_MONITOR, "true",
      nullptr);

  if (m_captureSink.load(std::memory_order_relaxed)) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
  }

  if (!m_targetObject.isEmpty()) {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, m_targetObject.toUtf8().constData());
  }

  m_stream = pw_stream_new(m_pw->core(), "Headroom Recorder", props);
  if (!m_stream) {
    return;
  }

  static const pw_stream_events streamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = &AudioRecorder::onStreamStateChanged;
    e.param_changed = &AudioRecorder::onStreamParamChanged;
    e.process = &AudioRecorder::onStreamProcess;
    return e;
  }();
  pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);

  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = sampleRate();
  info.channels = channels();
  if (info.channels >= 1) {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
  }
  if (info.channels >= 2) {
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }

  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const spa_pod* params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

  const int res = pw_stream_connect(
      m_stream,
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
      params,
      1);

  if (res < 0) {
    setErrorAndScheduleStop(QStringLiteral("pw_stream_connect failed: %1").arg(QString::fromUtf8(spa_strerror(res))));
    pw_stream_destroy(m_stream);
    m_stream = nullptr;
  }
}

void AudioRecorder::destroyStreamLocked()
{
  if (!m_stream) {
    return;
  }
  spa_hook_remove(&m_streamListener);
  pw_stream_destroy(m_stream);
  m_stream = nullptr;
  m_streamState.store(PW_STREAM_STATE_UNCONNECTED);
}

void AudioRecorder::processBuffer(pw_buffer* buffer)
{
  if (!buffer || !buffer->buffer || !m_sndFile) {
    return;
  }

  spa_buffer* b = buffer->buffer;
  if (b->n_datas < 1) {
    return;
  }

  spa_data& d = b->datas[0];
  if (!d.data || !d.chunk) {
    return;
  }

  const uint8_t* base = static_cast<const uint8_t*>(d.data);
  const uint32_t offset = d.chunk->offset;
  const uint32_t size = d.chunk->size;
  if (offset + size > d.maxsize) {
    return;
  }

  const uint32_t ch = std::max<uint32_t>(1u, channels());
  const uint32_t bytesPerFrame = ch * sizeof(float);
  if (bytesPerFrame == 0) {
    return;
  }

  const uint32_t framesU32 = size / bytesPerFrame;
  if (framesU32 == 0) {
    return;
  }

  m_lastBufferFrames.store(framesU32);
  m_lastBufferBytes.store(size);
  uint32_t expected = 0;
  (void)m_quantumFrames.compare_exchange_strong(expected, framesU32);

  const float* samples = reinterpret_cast<const float*>(base + offset);
  const sf_count_t frames = static_cast<sf_count_t>(framesU32);

  // Levels (best-effort).
  const size_t sampleCount = static_cast<size_t>(framesU32) * static_cast<size_t>(ch);
  float peak = 0.0f;
  double sumSq = 0.0;
  for (size_t i = 0; i < sampleCount; ++i) {
    const float s = samples[i];
    const float a = std::abs(s);
    peak = std::max(peak, a);
    sumSq += static_cast<double>(s) * static_cast<double>(s);
  }
  const float rms = sampleCount > 0 ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(sampleCount))) : 0.0f;
  m_peakDb.store(amplitudeToDb(peak));
  m_rmsDb.store(amplitudeToDb(rms));

  const sf_count_t wrote = sf_writef_float(m_sndFile, samples, frames);
  if (wrote > 0) {
    m_framesCaptured.fetch_add(static_cast<uint64_t>(wrote));
    const uint64_t rawBytes = static_cast<uint64_t>(wrote) * static_cast<uint64_t>(ch) * static_cast<uint64_t>(sizeof(float));
    m_dataBytesWritten.fetch_add(rawBytes);
  }

  if (wrote != frames) {
    setErrorAndScheduleStop(tr("Write failed: %1").arg(QString::fromLocal8Bit(sf_strerror(m_sndFile))));
  }
}

void AudioRecorder::setErrorAndScheduleStop(QString message)
{
  {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = message;
  }

  emit errorOccurred(message);

  bool expected = false;
  if (!m_stopScheduled.compare_exchange_strong(expected, true)) {
    return;
  }

  QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
}
