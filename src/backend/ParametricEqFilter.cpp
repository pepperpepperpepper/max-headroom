#include "ParametricEqFilter.h"

#include "backend/PipeWireThread.h"

#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/utils/result.h>

#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;

uint32_t spaChannelFromName(const QString& name)
{
  const QString n = name.trimmed().toUpper();
  if (n == QStringLiteral("FL")) {
    return SPA_AUDIO_CHANNEL_FL;
  }
  if (n == QStringLiteral("FR")) {
    return SPA_AUDIO_CHANNEL_FR;
  }
  if (n == QStringLiteral("FC")) {
    return SPA_AUDIO_CHANNEL_FC;
  }
  if (n == QStringLiteral("LFE")) {
    return SPA_AUDIO_CHANNEL_LFE;
  }
  if (n == QStringLiteral("RL")) {
    return SPA_AUDIO_CHANNEL_RL;
  }
  if (n == QStringLiteral("RR")) {
    return SPA_AUDIO_CHANNEL_RR;
  }
  if (n == QStringLiteral("SL")) {
    return SPA_AUDIO_CHANNEL_SL;
  }
  if (n == QStringLiteral("SR")) {
    return SPA_AUDIO_CHANNEL_SR;
  }
  return SPA_AUDIO_CHANNEL_UNKNOWN;
}

double dbToLin(double db)
{
  return std::pow(10.0, db / 20.0);
}

struct PortUserData final {
  ParametricEqFilter* self = nullptr;
  int channelIndex = 0;
  bool isInput = false;
};
} // namespace

ParametricEqFilter::ParametricEqFilter(PipeWireThread* pw,
                                       QString nodeName,
                                       QString nodeDescription,
                                       QVector<Channel> channels,
                                       QObject* parent)
    : QObject(parent)
    , m_pw(pw)
    , m_nodeName(std::move(nodeName))
    , m_nodeDescription(std::move(nodeDescription))
    , m_channels(std::move(channels))
{
  for (auto& ch : m_channels) {
    if (ch.spaChannel == 0) {
      ch.spaChannel = spaChannelFromName(ch.name);
    }
  }

  m_preset = defaultEqPreset(6);

  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  connectLocked();
  pw_thread_loop_unlock(loop);
}

ParametricEqFilter::~ParametricEqFilter()
{
  if (!m_pw || !m_pw->threadLoop()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);
  destroyLocked();
  pw_thread_loop_unlock(loop);
}

EqPreset ParametricEqFilter::preset() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_preset;
}

void ParametricEqFilter::setPreset(const EqPreset& preset)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_preset = preset;
  rebuildCoefficientsLocked();
}

void ParametricEqFilter::connectLocked()
{
  if (m_filter || !m_pw || !m_pw->core()) {
    return;
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE,
      "Audio",
      PW_KEY_MEDIA_CATEGORY,
      "Filter",
      PW_KEY_MEDIA_ROLE,
      "DSP",
      PW_KEY_MEDIA_CLASS,
      "Audio/Filter",
      PW_KEY_NODE_NAME,
      m_nodeName.toUtf8().constData(),
      PW_KEY_NODE_DESCRIPTION,
      m_nodeDescription.toUtf8().constData(),
      PW_KEY_NODE_VIRTUAL,
      "true",
      nullptr);

  m_filter = pw_filter_new(m_pw->core(), "Headroom EQ", props);
  if (!m_filter) {
    return;
  }

  static const pw_filter_events filterEvents = [] {
    pw_filter_events e{};
    e.version = PW_VERSION_FILTER_EVENTS;
    e.state_changed = &ParametricEqFilter::onFilterStateChanged;
    e.param_changed = &ParametricEqFilter::onFilterParamChanged;
    e.process = &ParametricEqFilter::onFilterProcess;
    return e;
  }();
  pw_filter_add_listener(m_filter, &m_filterListener, &filterEvents, this);

  m_ports.resize(m_channels.size());
  for (int i = 0; i < m_channels.size(); ++i) {
    const Channel ch = m_channels[i];
    const QString chName = ch.name.isEmpty() ? QStringLiteral("CH%1").arg(i + 1) : ch.name;

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = static_cast<uint32_t>(std::lround(m_sampleRate));
    info.channels = 1;
    info.position[0] = static_cast<spa_audio_channel>(ch.spaChannel);

    const spa_pod* params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    pw_properties* inProps = pw_properties_new(nullptr, nullptr);
    pw_properties_set(inProps, PW_KEY_PORT_NAME, QStringLiteral("in_%1").arg(chName).toUtf8().constData());
    pw_properties_set(inProps, PW_KEY_AUDIO_CHANNEL, chName.toUtf8().constData());
    auto* inData = static_cast<PortUserData*>(
        pw_filter_add_port(m_filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortUserData), inProps, params, 1));
    if (inData) {
      inData->self = this;
      inData->channelIndex = i;
      inData->isInput = true;
    }

    pw_properties* outProps = pw_properties_new(nullptr, nullptr);
    pw_properties_set(outProps, PW_KEY_PORT_NAME, QStringLiteral("out_%1").arg(chName).toUtf8().constData());
    pw_properties_set(outProps, PW_KEY_AUDIO_CHANNEL, chName.toUtf8().constData());
    auto* outData = static_cast<PortUserData*>(
        pw_filter_add_port(m_filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortUserData), outProps, params, 1));
    if (outData) {
      outData->self = this;
      outData->channelIndex = i;
      outData->isInput = false;
    }

    m_ports[i].inPort = inData;
    m_ports[i].outPort = outData;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    rebuildCoefficientsLocked();
  }

  const int res = pw_filter_connect(m_filter, PW_FILTER_FLAG_NONE, nullptr, 0);
  if (res < 0) {
    destroyLocked();
    return;
  }

  m_nodeId.store(pw_filter_get_node_id(m_filter));
}

void ParametricEqFilter::destroyLocked()
{
  if (!m_filter) {
    return;
  }

  spa_hook_remove(&m_filterListener);
  pw_filter_destroy(m_filter);
  m_filter = nullptr;
  m_ports.clear();
  m_nodeId.store(0);
}

void ParametricEqFilter::onFilterStateChanged(void* data, enum pw_filter_state /*old*/, enum pw_filter_state /*state*/, const char* /*error*/)
{
  auto* self = static_cast<ParametricEqFilter*>(data);
  if (!self) {
    return;
  }
}

void ParametricEqFilter::onFilterParamChanged(void* data, void* /*port_data*/, uint32_t id, const struct spa_pod* param)
{
  auto* self = static_cast<ParametricEqFilter*>(data);
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

  if (info.rate == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(self->m_mutex);
  self->m_sampleRate = static_cast<double>(info.rate);
  self->rebuildCoefficientsLocked();
}

void ParametricEqFilter::onFilterProcess(void* data, struct spa_io_position* /*position*/)
{
  auto* self = static_cast<ParametricEqFilter*>(data);
  if (!self) {
    return;
  }
  std::lock_guard<std::mutex> lock(self->m_mutex);
  self->processLocked();
}

double ParametricEqFilter::clamp(double v, double lo, double hi)
{
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

ParametricEqFilter::Biquad ParametricEqFilter::makeBiquad(EqBandType type, double sampleRate, double freqHz, double q, double gainDb)
{
  Biquad c;
  if (sampleRate <= 0.0) {
    return c;
  }

  const double fs = sampleRate;
  const double f0 = clamp(freqHz, 1.0, fs * 0.49);
  const double Q = clamp(q, 0.05, 24.0);

  const double w0 = 2.0 * kPi * (f0 / fs);
  const double cs = std::cos(w0);
  const double sn = std::sin(w0);
  const double alpha = sn / (2.0 * Q);

  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

  switch (type) {
    case EqBandType::Peaking: {
      const double A = std::pow(10.0, gainDb / 40.0);
      b0 = 1.0 + alpha * A;
      b1 = -2.0 * cs;
      b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha / A;
      break;
    }
    case EqBandType::LowPass: {
      b0 = (1.0 - cs) * 0.5;
      b1 = 1.0 - cs;
      b2 = (1.0 - cs) * 0.5;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::HighPass: {
      b0 = (1.0 + cs) * 0.5;
      b1 = -(1.0 + cs);
      b2 = (1.0 + cs) * 0.5;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::Notch: {
      b0 = 1.0;
      b1 = -2.0 * cs;
      b2 = 1.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::BandPass: {
      b0 = alpha;
      b1 = 0.0;
      b2 = -alpha;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::LowShelf: {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double S = clamp(q, 0.05, 24.0);
      const double alphaS = sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
      const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alphaS;

      b0 = A * ((A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
      b2 = A * ((A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha);
      a0 = (A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
      a2 = (A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha;
      break;
    }
    case EqBandType::HighShelf: {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double S = clamp(q, 0.05, 24.0);
      const double alphaS = sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
      const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alphaS;

      b0 = A * ((A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
      b2 = A * ((A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha);
      a0 = (A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
      a2 = (A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha;
      break;
    }
  }

  if (a0 == 0.0) {
    return c;
  }

  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

void ParametricEqFilter::rebuildCoefficientsLocked()
{
  const int channels = m_channels.size();
  const int bands = m_preset.bands.size();

  m_biquads.resize(channels);
  m_biquadState.resize(channels);
  for (int ch = 0; ch < channels; ++ch) {
    m_biquads[ch].resize(bands);
    m_biquadState[ch].resize(bands);
  }

  for (int i = 0; i < bands; ++i) {
    const EqBand band = m_preset.bands[i];
    const Biquad biq = makeBiquad(band.type, m_sampleRate, band.freqHz, band.q, band.gainDb);
    for (int ch = 0; ch < channels; ++ch) {
      m_biquads[ch][i] = biq;
      m_biquadState[ch][i] = BiquadState{};
    }
  }
}

void ParametricEqFilter::processLocked()
{
  if (!m_filter) {
    return;
  }

  const bool enabled = m_preset.enabled;
  const double pre = enabled ? dbToLin(m_preset.preampDb) : 1.0;

  const int channels = m_ports.size();
  for (int ch = 0; ch < channels; ++ch) {
    void* inPort = m_ports[ch].inPort;
    void* outPort = m_ports[ch].outPort;
    if (!inPort || !outPort) {
      continue;
    }

    pw_buffer* inBuf = pw_filter_dequeue_buffer(inPort);
    pw_buffer* outBuf = pw_filter_dequeue_buffer(outPort);
    if (!inBuf || !outBuf) {
      if (inBuf) {
        pw_filter_queue_buffer(inPort, inBuf);
      }
      if (outBuf) {
        pw_filter_queue_buffer(outPort, outBuf);
      }
      continue;
    }

    spa_buffer* inSpa = inBuf->buffer;
    spa_buffer* outSpa = outBuf->buffer;
    if (!inSpa || !outSpa || inSpa->n_datas < 1 || outSpa->n_datas < 1) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    spa_data& inD = inSpa->datas[0];
    spa_data& outD = outSpa->datas[0];
    if (!inD.data || !inD.chunk || !outD.data || !outD.chunk) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    const uint32_t inOffset = inD.chunk->offset;
    const uint32_t inSize = inD.chunk->size;
    const uint32_t outOffset = outD.chunk->offset;
    const uint32_t outMax = outD.maxsize;
    if (inOffset + inSize > inD.maxsize || outOffset >= outMax) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    const auto* inF = reinterpret_cast<const float*>(static_cast<const uint8_t*>(inD.data) + inOffset);
    auto* outF = reinterpret_cast<float*>(static_cast<uint8_t*>(outD.data) + outOffset);

    const uint32_t inFrames = inSize / sizeof(float);
    const uint32_t outFrames = (outMax - outOffset) / sizeof(float);
    const uint32_t frames = std::min(inFrames, outFrames);
    if (frames == 0) {
      pw_filter_queue_buffer(inPort, inBuf);
      pw_filter_queue_buffer(outPort, outBuf);
      continue;
    }

    if (!enabled) {
      for (uint32_t i = 0; i < frames; ++i) {
        outF[i] = inF[i];
      }
    } else {
      const int bands = m_preset.bands.size();
      for (uint32_t i = 0; i < frames; ++i) {
        double x = static_cast<double>(inF[i]) * pre;
        double y = x;
        for (int b = 0; b < bands; ++b) {
          const EqBand band = m_preset.bands[b];
          if (!band.enabled) {
            continue;
          }

          const Biquad c = m_biquads[ch][b];
          BiquadState& st = m_biquadState[ch][b];

          const double out = c.b0 * y + st.z1;
          st.z1 = c.b1 * y - c.a1 * out + st.z2;
          st.z2 = c.b2 * y - c.a2 * out;
          y = out;
        }
        outF[i] = static_cast<float>(y);
      }
    }

    outD.chunk->offset = outOffset;
    outD.chunk->size = frames * sizeof(float);
    outD.chunk->stride = sizeof(float);

    pw_filter_queue_buffer(inPort, inBuf);
    pw_filter_queue_buffer(outPort, outBuf);
  }
}

