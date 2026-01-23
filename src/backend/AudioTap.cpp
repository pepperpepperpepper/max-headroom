#include "AudioTap.h"

#include "PipeWireThread.h"
#include "dsp/Fft.h"
#include "settings/VisualizerSettings.h"

#include <QMetaObject>

#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/param/param.h>
#include <spa/utils/result.h>

#include <algorithm>
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

float clamp01(float v)
{
  if (v < 0.0f) {
    return 0.0f;
  }
  if (v > 1.0f) {
    return 1.0f;
  }
  return v;
}

std::size_t nearestPowerOfTwo(std::size_t n)
{
  if (n < 2) {
    return 2;
  }
  std::size_t p = 1;
  while (p < n && p < (1U << 30U)) {
    p <<= 1U;
  }
  const std::size_t lower = p >> 1U;
  if (p == n || lower == 0) {
    return p;
  }
  return (n - lower < p - n) ? lower : p;
}

std::size_t sanitizeFftSize(int fftSize)
{
  const std::size_t minSize = 256;
  const std::size_t maxSize = 16384;
  std::size_t n = fftSize > 0 ? static_cast<std::size_t>(fftSize) : 1024U;
  n = std::clamp(n, minSize, maxSize);
  if (!dsp::Fft::isPowerOfTwo(n)) {
    n = nearestPowerOfTwo(n);
    n = std::clamp(n, minSize, maxSize);
  }
  return n;
}
} // namespace

AudioTap::AudioTap(PipeWireThread* pw, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
{
  {
    QSettings s;
    const VisualizerSettings cfg = VisualizerSettingsStore::load(s);
    applySettings(cfg);
  }

  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  connectStreamLocked();
  pw_thread_loop_unlock(loop);
}

AudioTap::~AudioTap()
{
  if (!m_pw || !m_pw->threadLoop()) {
    return;
  }
  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  destroyStreamLocked();
  pw_thread_loop_unlock(loop);
}

uint32_t AudioTap::sampleRate() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_sampleRate;
}

void AudioTap::applySettings(const VisualizerSettings& settings)
{
  const std::size_t nextFftSize = sanitizeFftSize(settings.fftSize);
  const float nextSmoothing = static_cast<float>(std::clamp(settings.spectrumSmoothing, 0.0, 0.999));
  const double nextWaveSec = std::clamp(settings.waveformHistorySeconds, 0.05, 30.0);
  const double nextSpecSec = std::clamp(settings.spectrogramHistorySeconds, 0.25, 120.0);

  std::lock_guard<std::mutex> lock(m_mutex);

  const bool fftChanged = nextFftSize != m_fftSize;
  const bool waveChanged = std::abs(nextWaveSec - m_waveHistorySeconds) > 1.0e-9;
  const bool specChanged = std::abs(nextSpecSec - m_spectrogramHistorySeconds) > 1.0e-9;

  m_spectrumSmoothing = nextSmoothing;
  m_waveHistorySeconds = nextWaveSec;
  m_spectrogramHistorySeconds = nextSpecSec;

  if (fftChanged) {
    m_fftSize = nextFftSize;
    m_hopSize = std::max<std::size_t>(1U, m_fftSize / 4U);
  }

  if (m_hann.size() != m_fftSize) {
    m_hann = dsp::Fft::hannWindow(m_fftSize);
  }
  if (m_fftRing.size() != m_fftSize) {
    m_fftRing.assign(m_fftSize, 0.0f);
    m_fftWrite = 0;
    m_fftTotal = 0;
    m_sinceFrame = 0;
  }

  if (waveChanged || m_waveRing.empty()) {
    resizeWaveRingLocked();
  }
  if (fftChanged || specChanged || m_specPixels.isEmpty() || m_spectrum.isEmpty()) {
    resizeSpectrogramLocked();
  }
}

void AudioTap::setTargetObject(const QString& targetObject)
{
  setTarget(m_captureSink.load(std::memory_order_relaxed), targetObject);
}

void AudioTap::setCaptureSink(bool captureSink)
{
  setTarget(captureSink, m_targetObject);
}

void AudioTap::setTarget(bool captureSink, const QString& targetObject)
{
  const bool prevCaptureSink = m_captureSink.load(std::memory_order_relaxed);
  if (captureSink == prevCaptureSink && targetObject == m_targetObject) {
    return;
  }

  m_captureSink.store(captureSink, std::memory_order_relaxed);
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

void AudioTap::connectStreamLocked()
{
  if (m_stream || !m_pw || !m_pw->core()) {
    return;
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Monitor",
      PW_KEY_NODE_NAME, "headroom.visualizer",
      PW_KEY_STREAM_MONITOR, "true",
      nullptr);

  if (m_captureSink.load(std::memory_order_relaxed)) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
  }

  if (!m_targetObject.isEmpty()) {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, m_targetObject.toUtf8().constData());
  }

  m_stream = pw_stream_new(m_pw->core(), "Headroom Visualizer", props);
  if (!m_stream) {
    return;
  }

  static const pw_stream_events streamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = &AudioTap::onStreamStateChanged;
    e.param_changed = &AudioTap::onStreamParamChanged;
    e.process = &AudioTap::onStreamProcess;
    return e;
  }();
  pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);

  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = m_sampleRate;
  info.channels = m_channels;
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

void AudioTap::destroyStreamLocked()
{
  if (!m_stream) {
    return;
  }
  spa_hook_remove(&m_streamListener);
  pw_stream_destroy(m_stream);
  m_stream = nullptr;
}

void AudioTap::resizeWaveRingLocked()
{
  const double sec = std::clamp(m_waveHistorySeconds, 0.05, 30.0);
  const std::size_t want = std::max<std::size_t>(
      1U,
      static_cast<std::size_t>(std::llround(sec * static_cast<double>(m_sampleRate == 0 ? 48000U : m_sampleRate))));

  if (m_waveRing.size() != want) {
    m_waveRing.assign(want, 0.0f);
    m_waveWrite = 0;
    m_waveCount = 0;
  }
}

void AudioTap::resizeSpectrogramLocked()
{
  const int bins = std::max<int>(1, static_cast<int>(m_fftSize / 2U));
  m_specBins = bins;

  const double rate = static_cast<double>(m_sampleRate == 0 ? 48000U : m_sampleRate);
  const double hop = static_cast<double>(m_hopSize == 0 ? 256U : m_hopSize);
  const int wantColumns = static_cast<int>(std::lround(std::clamp(m_spectrogramHistorySeconds, 0.25, 120.0) * rate / hop));
  m_specColumns = std::clamp(wantColumns, 30, 4000);

  m_spectrum.fill(0.0f, m_specBins);
  m_specPixels.fill(0, m_specColumns * m_specBins);
  m_specWriteCol = 0;
}

void AudioTap::onStreamStateChanged(void* data, enum pw_stream_state /*old*/, enum pw_stream_state state, const char* error)
{
  auto* self = static_cast<AudioTap*>(data);
  const QString msg = (state == PW_STREAM_STATE_ERROR && error)
      ? QStringLiteral("%1: %2").arg(QString::fromUtf8(stateToString(state)), QString::fromUtf8(error))
      : QString::fromUtf8(stateToString(state));

  QMetaObject::invokeMethod(self, [self, msg]() { emit self->streamStateChanged(msg); }, Qt::QueuedConnection);
}

void AudioTap::onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param)
{
  auto* self = static_cast<AudioTap*>(data);
  if (!param) {
    return;
  }
  if (id != SPA_PARAM_Format) {
    return;
  }

  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(self->m_mutex);
  const uint32_t prevRate = self->m_sampleRate;
  if (info.rate > 0) {
    self->m_sampleRate = info.rate;
  }
  if (info.channels > 0) {
    self->m_channels = info.channels;
  }

  if (self->m_sampleRate != prevRate) {
    self->resizeWaveRingLocked();
    self->resizeSpectrogramLocked();
  }
}

void AudioTap::onStreamProcess(void* data)
{
  auto* self = static_cast<AudioTap*>(data);
  if (!self->m_stream) {
    return;
  }
  pw_buffer* buffer = pw_stream_dequeue_buffer(self->m_stream);
  if (!buffer) {
    return;
  }
  self->processBuffer(buffer);
  pw_stream_queue_buffer(self->m_stream, buffer);
}

void AudioTap::processBuffer(pw_buffer* buffer)
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

  const uint32_t channels = m_channels == 0 ? 1U : m_channels;
  const uint8_t* base = static_cast<const uint8_t*>(d.data);
  const uint32_t offset = d.chunk->offset;
  const uint32_t size = d.chunk->size;
  if (offset + size > d.maxsize) {
    return;
  }

  const float* samples = reinterpret_cast<const float*>(base + offset);
  const std::size_t nFloats = size / sizeof(float);
  const std::size_t frames = nFloats / channels;
  if (frames == 0) {
    return;
  }

  std::vector<float> mono(frames);
  float peak = 0.0f;
  double sumSq = 0.0;
  for (std::size_t f = 0; f < frames; ++f) {
    float v = 0.0f;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      v += samples[f * channels + ch];
    }
    v /= static_cast<float>(channels);
    mono[f] = v;
    const float av = std::fabs(v);
    if (av > peak) {
      peak = av;
    }
    sumSq += static_cast<double>(v) * static_cast<double>(v);
  }
  const float rms = std::sqrt(sumSq / static_cast<double>(frames));
  m_peak.store(peak, std::memory_order_relaxed);
  m_rms.store(rms, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(m_mutex);

  // Update wave ring.
  for (float v : mono) {
    if (m_waveRing.empty()) {
      break;
    }
    m_waveRing[m_waveWrite] = v;
    m_waveWrite = (m_waveWrite + 1U) % m_waveRing.size();
    m_waveCount = std::min<std::size_t>(m_waveCount + 1U, m_waveRing.size());

    // Update FFT ring.
    if (m_fftRing.empty()) {
      continue;
    }
    m_fftRing[m_fftWrite] = v;
    m_fftWrite = (m_fftWrite + 1U) % m_fftSize;
    ++m_fftTotal;
    ++m_sinceFrame;

    if (m_fftTotal >= m_fftSize && m_sinceFrame >= m_hopSize) {
      m_sinceFrame = 0;

      std::vector<float> frame(m_fftSize);
      for (std::size_t i = 0; i < m_fftSize; ++i) {
        const std::size_t idx = (m_fftWrite + i) % m_fftSize;
        frame[i] = m_fftRing[idx] * m_hann[i];
      }

      auto spectrum = dsp::Fft::forwardReal(frame);
      const int bins = static_cast<int>(m_fftSize / 2);
      if (bins <= 0) {
        continue;
      }

      if (m_specBins != bins) {
        m_specBins = bins;
        m_spectrum.fill(0.0f, m_specBins);
        m_specPixels.fill(0, m_specColumns * m_specBins);
        m_specWriteCol = 0;
      }

      // Update spectrum + spectrogram column.
      constexpr float minDb = -90.0f;
      constexpr float maxDb = 0.0f;
      for (int i = 0; i < bins; ++i) {
        const float mag = std::abs(spectrum[static_cast<std::size_t>(i)]) / static_cast<float>(bins);
        const float db = 20.0f * std::log10(mag + 1.0e-9f);
        const float norm = clamp01((db - minDb) / (maxDb - minDb));

        // Simple peak-hold-ish smoothing for the spectrum.
        const float prev = (i < m_spectrum.size()) ? m_spectrum[i] : 0.0f;
        const float s = std::clamp(m_spectrumSmoothing, 0.0f, 0.999f);
        const float next = (norm > prev) ? norm : (prev * s + norm * (1.0f - s));
        if (i < m_spectrum.size()) {
          m_spectrum[i] = next;
        }

        const int col = m_specWriteCol;
        const int row = i;
        const int index = col * m_specBins + row;
        if (index >= 0 && index < m_specPixels.size()) {
          m_specPixels[index] = spectrogramColor(norm);
        }
      }

      m_specWriteCol = (m_specWriteCol + 1) % m_specColumns;
    }
  }
}

QVector<float> AudioTap::spectrum() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_spectrum;
}

SpectrogramSnapshot AudioTap::spectrogram() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  SpectrogramSnapshot snap;
  snap.columns = m_specColumns;
  snap.bins = m_specBins;
  snap.writeColumn = m_specWriteCol;
  snap.pixels = m_specPixels;
  return snap;
}

void AudioTap::waveformMinMax(int columns, int windowSamples, QVector<float>& minsOut, QVector<float>& maxsOut) const
{
  minsOut.clear();
  maxsOut.clear();
  if (columns <= 0 || windowSamples <= 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_waveRing.empty() || m_waveCount == 0) {
    return;
  }

  const std::size_t available = m_waveCount;
  const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(windowSamples), available);
  const std::size_t start = (m_waveWrite + m_waveRing.size() - take) % m_waveRing.size();

  minsOut.resize(columns);
  maxsOut.resize(columns);

  for (int x = 0; x < columns; ++x) {
    float minV = 1.0f;
    float maxV = -1.0f;
    const std::size_t segStart = (take * static_cast<std::size_t>(x)) / static_cast<std::size_t>(columns);
    const std::size_t segEnd = (take * static_cast<std::size_t>(x + 1)) / static_cast<std::size_t>(columns);
    const std::size_t count = std::max<std::size_t>(1U, segEnd - segStart);

    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t idx = (start + segStart + i) % m_waveRing.size();
      const float v = m_waveRing[idx];
      if (v < minV) {
        minV = v;
      }
      if (v > maxV) {
        maxV = v;
      }
    }

    minsOut[x] = minV;
    maxsOut[x] = maxV;
  }
}

uint32_t AudioTap::spectrogramColor(float normalized01)
{
  const float t = clamp01(normalized01);

  auto lerp = [](float a, float b, float x) { return a + (b - a) * x; };

  // A small hand-tuned palette: deep purple -> blue -> cyan -> yellow.
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  if (t < 0.33f) {
    const float x = t / 0.33f;
    r = lerp(20.0f, 30.0f, x);
    g = lerp(0.0f, 70.0f, x);
    b = lerp(60.0f, 200.0f, x);
  } else if (t < 0.66f) {
    const float x = (t - 0.33f) / 0.33f;
    r = lerp(30.0f, 20.0f, x);
    g = lerp(70.0f, 220.0f, x);
    b = lerp(200.0f, 240.0f, x);
  } else {
    const float x = (t - 0.66f) / 0.34f;
    r = lerp(20.0f, 255.0f, x);
    g = lerp(220.0f, 230.0f, x);
    b = lerp(240.0f, 60.0f, x);
  }

  const uint32_t R = static_cast<uint32_t>(clamp01(r / 255.0f) * 255.0f);
  const uint32_t G = static_cast<uint32_t>(clamp01(g / 255.0f) * 255.0f);
  const uint32_t B = static_cast<uint32_t>(clamp01(b / 255.0f) * 255.0f);
  return (0xFFU << 24U) | (R << 16U) | (G << 8U) | B;
}
