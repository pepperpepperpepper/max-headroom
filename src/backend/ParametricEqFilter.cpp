#include "ParametricEqFilter.h"

#include "backend/PipeWireThread.h"

#include <QRegularExpression>

#include <pipewire/keys.h>
#include <pipewire/properties.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>

namespace {
bool isWellKnownChannelLabel(const QString& label)
{
  const QString n = label.trimmed().toUpper();
  if (n == QStringLiteral("FL") || n == QStringLiteral("FR") || n == QStringLiteral("FC") || n == QStringLiteral("LFE")
      || n == QStringLiteral("RL") || n == QStringLiteral("RR") || n == QStringLiteral("SL") || n == QStringLiteral("SR")
      || n == QStringLiteral("MONO")) {
    return true;
  }
  return false;
}

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

  // Try to infer a standard channel label from common port naming schemes like
  // "playback_FL", "monitor_FR", "capture_FL", etc.
  {
    const QStringList parts = n.split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")), Qt::SkipEmptyParts);
    for (const auto& part : parts) {
      if (part == QStringLiteral("FL")) {
        return SPA_AUDIO_CHANNEL_FL;
      }
      if (part == QStringLiteral("FR")) {
        return SPA_AUDIO_CHANNEL_FR;
      }
      if (part == QStringLiteral("FC")) {
        return SPA_AUDIO_CHANNEL_FC;
      }
      if (part == QStringLiteral("LFE")) {
        return SPA_AUDIO_CHANNEL_LFE;
      }
      if (part == QStringLiteral("RL")) {
        return SPA_AUDIO_CHANNEL_RL;
      }
      if (part == QStringLiteral("RR")) {
        return SPA_AUDIO_CHANNEL_RR;
      }
      if (part == QStringLiteral("SL")) {
        return SPA_AUDIO_CHANNEL_SL;
      }
      if (part == QStringLiteral("SR")) {
        return SPA_AUDIO_CHANNEL_SR;
      }
    }
  }

  // Many virtual nodes expose a single port named "playback_1"/"capture_1" with no channel metadata.
  // Use MONO instead of UNKNOWN so WirePlumber can negotiate a usable format.
  return SPA_AUDIO_CHANNEL_MONO;
}

struct PortUserData final {
  ParametricEqFilter* self = nullptr;
  int portIndex = 0;
  bool isInput = false;
};
} // namespace

ParametricEqFilter::ParametricEqFilter(PipeWireThread* pw,
                                       QString nodeName,
                                       QString nodeDescription,
                                       QVector<PortSpec> ports,
                                       QObject* parent)
    : QObject(parent)
    , m_pw(pw)
    , m_nodeName(std::move(nodeName))
    , m_nodeDescription(std::move(nodeDescription))
    , m_portSpecs(std::move(ports))
{
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

  m_ports.resize(m_portSpecs.size());
  int totalChannels = 0;
  for (int i = 0; i < m_portSpecs.size(); ++i) {
    const PortSpec spec = m_portSpecs[i];
    const QString key = spec.key.trimmed().isEmpty() ? QStringLiteral("port%1").arg(i + 1) : spec.key.trimmed();

    QVector<QString> channelLabels;
    channelLabels.reserve(spec.channels.size());
    for (const auto& raw : spec.channels) {
      const QString label = raw.trimmed();
      if (!label.isEmpty()) {
        channelLabels.push_back(label);
      }
    }
    if (channelLabels.isEmpty()) {
      channelLabels = {QStringLiteral("FL"), QStringLiteral("FR")};
    }

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	    spa_audio_info_raw info{};
	    info.format = SPA_AUDIO_FORMAT_F32;
	    info.rate = static_cast<uint32_t>(std::lround(m_sampleRate));
	    const int channelCount = std::max(1, static_cast<int>(channelLabels.size()));
	    info.channels = static_cast<uint32_t>(channelCount);
	    const int chCount = std::min<int>(SPA_AUDIO_MAX_CHANNELS, channelCount);
	    for (int ch = 0; ch < chCount; ++ch) {
	      info.position[ch] = static_cast<spa_audio_channel>(spaChannelFromName(channelLabels[ch]));
	    }

    const spa_pod* params[] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

	    pw_properties* inProps = pw_properties_new(nullptr, nullptr);
	    pw_properties_set(inProps, PW_KEY_PORT_NAME, QStringLiteral("in_%1").arg(key).toUtf8().constData());
	    if (channelLabels.size() == 1 && isWellKnownChannelLabel(channelLabels[0])) {
	      pw_properties_set(inProps, PW_KEY_AUDIO_CHANNEL, channelLabels[0].toUtf8().constData());
	    }
    auto* inData = static_cast<PortUserData*>(
        pw_filter_add_port(m_filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortUserData), inProps, params, 1));
    if (inData) {
      inData->self = this;
      inData->portIndex = i;
      inData->isInput = true;
    }

	    pw_properties* outProps = pw_properties_new(nullptr, nullptr);
	    pw_properties_set(outProps, PW_KEY_PORT_NAME, QStringLiteral("out_%1").arg(key).toUtf8().constData());
	    if (channelLabels.size() == 1 && isWellKnownChannelLabel(channelLabels[0])) {
	      pw_properties_set(outProps, PW_KEY_AUDIO_CHANNEL, channelLabels[0].toUtf8().constData());
	    }
    auto* outData = static_cast<PortUserData*>(
        pw_filter_add_port(m_filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortUserData), outProps, params, 1));
    if (outData) {
      outData->self = this;
      outData->portIndex = i;
      outData->isInput = false;
    }

    m_ports[i].inPort = inData;
    m_ports[i].outPort = outData;
    m_ports[i].channels = static_cast<int>(info.channels);
    m_ports[i].channelBase = totalChannels;
    totalChannels += static_cast<int>(info.channels);
  }
  m_totalChannels = totalChannels;

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
  m_totalChannels = 0;
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
