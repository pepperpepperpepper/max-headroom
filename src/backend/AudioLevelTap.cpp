#include "AudioLevelTap.h"

#include "PipeWireThread.h"

#include <QMetaObject>

#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
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

int64_t nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t nextTapSerial()
{
  static std::atomic<uint64_t> s_serial{1};
  return s_serial.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

AudioLevelTap::AudioLevelTap(PipeWireThread* pw, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
{
  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  connectStreamLocked();
  pw_thread_loop_unlock(loop);
}

AudioLevelTap::~AudioLevelTap()
{
  if (!m_pw || !m_pw->threadLoop()) {
    return;
  }
  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  destroyStreamLocked();
  pw_thread_loop_unlock(loop);
}

void AudioLevelTap::setCaptureSink(bool captureSink)
{
  if (captureSink == m_captureSink.load(std::memory_order_relaxed)) {
    return;
  }
  m_captureSink.store(captureSink, std::memory_order_relaxed);

  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  destroyStreamLocked();
  connectStreamLocked();
  pw_thread_loop_unlock(loop);
}

bool AudioLevelTap::clippedRecently(int holdMs) const
{
  if (holdMs <= 0) {
    return false;
  }
  const int64_t last = m_lastClipNs.load(std::memory_order_relaxed);
  if (last <= 0) {
    return false;
  }
  const int64_t ageNs = nowNs() - last;
  return ageNs >= 0 && ageNs < static_cast<int64_t>(holdMs) * 1'000'000LL;
}

void AudioLevelTap::setTargetObject(const QString& targetObject)
{
  if (targetObject == m_targetObject) {
    return;
  }
  m_targetObject = targetObject;

  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  destroyStreamLocked();
  connectStreamLocked();
  pw_thread_loop_unlock(loop);
}

void AudioLevelTap::connectStreamLocked()
{
  if (m_stream || !m_pw || !m_pw->core()) {
    return;
  }

  const QByteArray nodeName = QByteArray("headroom.meter.") + QByteArray::number(nextTapSerial());

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Monitor",
      PW_KEY_NODE_NAME, nodeName.constData(),
      PW_KEY_NODE_DESCRIPTION, "Headroom Meter",
      PW_KEY_STREAM_MONITOR, "true",
      nullptr);

  if (m_captureSink.load(std::memory_order_relaxed)) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
  }

  if (!m_targetObject.isEmpty()) {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, m_targetObject.toUtf8().constData());
  }

  m_stream = pw_stream_new(m_pw->core(), "Headroom Meter", props);
  if (!m_stream) {
    return;
  }

  static const pw_stream_events streamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = &AudioLevelTap::onStreamStateChanged;
    e.param_changed = &AudioLevelTap::onStreamParamChanged;
    e.process = &AudioLevelTap::onStreamProcess;
    return e;
  }();
  pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);

  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = 2;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;

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
    pw_stream_destroy(m_stream);
    m_stream = nullptr;
  }
}

void AudioLevelTap::destroyStreamLocked()
{
  if (!m_stream) {
    return;
  }
  spa_hook_remove(&m_streamListener);
  pw_stream_destroy(m_stream);
  m_stream = nullptr;
}

void AudioLevelTap::onStreamStateChanged(void* data, enum pw_stream_state /*old*/, enum pw_stream_state state, const char* error)
{
  auto* self = static_cast<AudioLevelTap*>(data);
  if (!self) {
    return;
  }

  const QString msg = (state == PW_STREAM_STATE_ERROR && error)
      ? QStringLiteral("%1: %2").arg(QString::fromUtf8(stateToString(state)), QString::fromUtf8(error))
      : QString::fromUtf8(stateToString(state));

  QMetaObject::invokeMethod(self, [self, msg]() { emit self->streamStateChanged(msg); }, Qt::QueuedConnection);
}

void AudioLevelTap::onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
  auto* self = static_cast<AudioLevelTap*>(data);
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

  if (info.channels > 0) {
    self->m_channels.store(info.channels, std::memory_order_relaxed);
  }
}

void AudioLevelTap::onStreamProcess(void* data)
{
  auto* self = static_cast<AudioLevelTap*>(data);
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

void AudioLevelTap::processBuffer(pw_buffer* buffer)
{
  if (!buffer || !buffer->buffer) {
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

  const float* samples = reinterpret_cast<const float*>(base + offset);
  const std::size_t nFloats = size / sizeof(float);
  if (nFloats == 0) {
    return;
  }

  // Compute on a mono mixdown of all negotiated channels.
  const uint32_t ch = std::max<uint32_t>(1U, m_channels.load(std::memory_order_relaxed));
  const std::size_t frames = nFloats / ch;
  if (frames == 0) {
    return;
  }

  float peak = 0.0f;
  double sumSq = 0.0;
  for (std::size_t f = 0; f < frames; ++f) {
    float v = 0.0f;
    for (uint32_t c = 0; c < ch; ++c) {
      v += samples[f * ch + c];
    }
    v /= static_cast<float>(ch);

    const float av = std::fabs(v);
    if (av > peak) {
      peak = av;
    }
    sumSq += static_cast<double>(v) * static_cast<double>(v);
  }

  const float rms = std::sqrt(sumSq / static_cast<double>(frames));
  m_peak.store(peak, std::memory_order_relaxed);
  m_rms.store(rms, std::memory_order_relaxed);

  if (peak >= 1.0f) {
    m_lastClipNs.store(nowNs(), std::memory_order_relaxed);
  }
}
