#include "PipeWireGraph.h"

#include "PipeWireThread.h"

#include <QByteArray>
#include <QMetaObject>
#include <QRegularExpression>

#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/extensions/profiler.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/module.h>
#include <pipewire/node.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>

#include <spa/param/profiler.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>

#include <algorithm>

namespace {
QString dictString(const spa_dict* dict, const char* key)
{
  if (!dict || !key) {
    return {};
  }
  if (const char* value = spa_dict_lookup(dict, key)) {
    return QString::fromUtf8(value);
  }
  return {};
}

std::optional<uint32_t> dictU32(const spa_dict* dict, const char* key)
{
  if (!dict || !key) {
    return std::nullopt;
  }
  const char* value = spa_dict_lookup(dict, key);
  if (!value) {
    return std::nullopt;
  }
  bool ok = false;
  const uint32_t parsed = QString::fromUtf8(value).toUInt(&ok);
  return ok ? std::optional<uint32_t>(parsed) : std::nullopt;
}
} // namespace

struct PipeWireGraph::NodeBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t nodeId = 0;
  pw_node* node = nullptr;
  spa_hook listener{};
};

struct PipeWireGraph::MetadataBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t metadataId = 0;
  QString name;
  pw_metadata* metadata = nullptr;
  spa_hook listener{};
};

struct PipeWireGraph::ProfilerBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t profilerId = 0;
  pw_profiler* profiler = nullptr;
  spa_hook listener{};
};

PipeWireGraph::PipeWireGraph(PipeWireThread* pw, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
{
  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  m_registry = pw_core_get_registry(m_pw->core(), PW_VERSION_REGISTRY, 0);
  if (!m_registry) {
    pw_thread_loop_unlock(loop);
    return;
  }

  static const pw_registry_events registryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = &PipeWireGraph::onRegistryGlobal;
    e.global_remove = &PipeWireGraph::onRegistryGlobalRemove;
    return e;
  }();
  pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);

  // Ensure we get the current global list (not only future changes).
  pw_core_sync(m_pw->core(), PW_ID_CORE, 0);

  pw_thread_loop_unlock(loop);
}

PipeWireGraph::~PipeWireGraph()
{
  if (!m_pw || !m_pw->threadLoop()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  {
    QHash<uint32_t, NodeBinding*> bindings;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      bindings = m_nodeBindings;
      m_nodeBindings.clear();
      m_nodeControls.clear();
    }

    for (auto* b : bindings) {
      if (!b) {
        continue;
      }
      spa_hook_remove(&b->listener);
      if (b->node) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(b->node));
      }
      delete b;
    }
  }

  {
    QHash<uint32_t, MetadataBinding*> bindings;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      bindings = m_metadataBindings;
      m_metadataBindings.clear();
      m_defaultDeviceMetadata = nullptr;
      m_settingsMetadata = nullptr;
      m_defaultAudioSinkId.reset();
      m_configuredAudioSinkId.reset();
      m_defaultAudioSourceId.reset();
      m_configuredAudioSourceId.reset();
      m_clockRate.reset();
      m_clockAllowedRates.clear();
      m_clockQuantum.reset();
      m_clockMinQuantum.reset();
      m_clockMaxQuantum.reset();
      m_clockForceRate.reset();
      m_clockForceQuantum.reset();
    }

    for (auto* b : bindings) {
      if (!b) {
        continue;
      }
      spa_hook_remove(&b->listener);
      if (b->metadata) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(b->metadata));
      }
      delete b;
    }
  }

  {
    ProfilerBinding* binding = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      binding = m_profilerBinding;
      m_profilerBinding = nullptr;
      m_profilerSnapshot.reset();
    }

    if (binding) {
      spa_hook_remove(&binding->listener);
      if (binding->profiler) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->profiler));
      }
      delete binding;
    }
  }

  if (m_registry) {
    spa_hook_remove(&m_registryListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
    m_registry = nullptr;
  }

  pw_thread_loop_unlock(loop);
}

QList<PwNodeInfo> PipeWireGraph::nodes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_nodes.values();
}

QList<PwPortInfo> PipeWireGraph::ports() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_ports.values();
}

QList<PwLinkInfo> PipeWireGraph::links() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_links.values();
}

QList<PwModuleInfo> PipeWireGraph::modules() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_modules.values();
}

std::optional<PwNodeInfo> PipeWireGraph::nodeById(uint32_t id) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_nodes.contains(id)) {
    return std::nullopt;
  }
  return m_nodes.value(id);
}

std::optional<PwNodeControls> PipeWireGraph::nodeControls(uint32_t nodeId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_nodeControls.contains(nodeId)) {
    return std::nullopt;
  }
  return m_nodeControls.value(nodeId);
}

QList<PwNodeInfo> PipeWireGraph::audioSources() const
{
  QList<PwNodeInfo> out;
  for (const auto& node : nodes()) {
    if (node.mediaClass == QStringLiteral("Audio/Source")) {
      out.push_back(node);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioSinks() const
{
  QList<PwNodeInfo> out;
  for (const auto& node : nodes()) {
    if (node.mediaClass == QStringLiteral("Audio/Sink")) {
      out.push_back(node);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioPlaybackStreams() const
{
  QList<PwNodeInfo> out;
  for (const auto& node : nodes()) {
    if (node.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
      out.push_back(node);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioCaptureStreams() const
{
  QList<PwNodeInfo> out;
  for (const auto& node : nodes()) {
    if (node.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
      out.push_back(node);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

bool PipeWireGraph::hasDefaultDeviceSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_defaultDeviceMetadata != nullptr && m_defaultDeviceMetadata->metadata != nullptr;
}

std::optional<uint32_t> PipeWireGraph::defaultAudioSinkId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_defaultAudioSinkId.has_value()) {
    return m_defaultAudioSinkId;
  }
  return m_configuredAudioSinkId;
}

std::optional<uint32_t> PipeWireGraph::defaultAudioSourceId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_defaultAudioSourceId.has_value()) {
    return m_defaultAudioSourceId;
  }
  return m_configuredAudioSourceId;
}

bool PipeWireGraph::setDefaultAudioSink(uint32_t nodeId)
{
  if (nodeId == 0 || !m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_defaultDeviceMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(nodeId);
  const int r1 = pw_metadata_set_property(binding->metadata, 0, "default.audio.sink", "Spa:Id", value.constData());
  const int r2 = pw_metadata_set_property(binding->metadata, 0, "default.configured.audio.sink", "Spa:Id", value.constData());

  pw_thread_loop_unlock(loop);
  return r1 >= 0 && r2 >= 0;
}

bool PipeWireGraph::setDefaultAudioSource(uint32_t nodeId)
{
  if (nodeId == 0 || !m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_defaultDeviceMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(nodeId);
  const int r1 = pw_metadata_set_property(binding->metadata, 0, "default.audio.source", "Spa:Id", value.constData());
  const int r2 = pw_metadata_set_property(binding->metadata, 0, "default.configured.audio.source", "Spa:Id", value.constData());

  pw_thread_loop_unlock(loop);
  return r1 >= 0 && r2 >= 0;
}

bool PipeWireGraph::hasClockSettingsSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_settingsMetadata != nullptr && m_settingsMetadata->metadata != nullptr;
}

PwClockSettings PipeWireGraph::clockSettings() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  PwClockSettings s;
  s.rate = m_clockRate;
  s.allowedRates = m_clockAllowedRates;
  s.quantum = m_clockQuantum;
  s.minQuantum = m_clockMinQuantum;
  s.maxQuantum = m_clockMaxQuantum;
  s.forceRate = m_clockForceRate;
  s.forceQuantum = m_clockForceQuantum;
  return s;
}

QVector<PwClockPreset> PipeWireGraph::clockPresets()
{
  return {
      {QStringLiteral("auto"), tr("Auto (session-managed)"), std::nullopt, std::nullopt},
      {QStringLiteral("ll-48k-64"), tr("Low latency — 48 kHz / 64"), 48000u, 64u},
      {QStringLiteral("ll-48k-128"), tr("Low latency — 48 kHz / 128"), 48000u, 128u},
      {QStringLiteral("balanced-48k-256"), tr("Balanced — 48 kHz / 256"), 48000u, 256u},
      {QStringLiteral("stable-48k-512"), tr("Stable — 48 kHz / 512"), 48000u, 512u},
      {QStringLiteral("hq-96k-256"), tr("High quality — 96 kHz / 256"), 96000u, 256u},
  };
}

bool PipeWireGraph::applyClockPreset(const QString& presetId)
{
  const QString id = presetId.trimmed().toLower();
  const auto presets = clockPresets();
  for (const auto& p : presets) {
    if (p.id == id) {
      const bool r1 = setClockForceRate(p.forceRate);
      const bool r2 = setClockForceQuantum(p.forceQuantum);
      return r1 && r2;
    }
  }
  return false;
}

bool PipeWireGraph::setClockForceRate(std::optional<uint32_t> rate)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(rate.value_or(0));
  const int res = pw_metadata_set_property(binding->metadata, 0, "clock.force-rate", "Spa:Int", value.constData());

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockForceQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(quantum.value_or(0));
  const int res = pw_metadata_set_property(binding->metadata, 0, "clock.force-quantum", "Spa:Int", value.constData());

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockMinQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = quantum.has_value() ? QByteArray::number(*quantum) : QByteArray();
  const int res = pw_metadata_set_property(binding->metadata,
                                           0,
                                           "clock.min-quantum",
                                           "Spa:Int",
                                           quantum.has_value() ? value.constData() : nullptr);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockMaxQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = quantum.has_value() ? QByteArray::number(*quantum) : QByteArray();
  const int res = pw_metadata_set_property(binding->metadata,
                                           0,
                                           "clock.max-quantum",
                                           "Spa:Int",
                                           quantum.has_value() ? value.constData() : nullptr);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::hasProfilerSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_profilerBinding != nullptr && m_profilerBinding->profiler != nullptr;
}

std::optional<PwProfilerSnapshot> PipeWireGraph::profilerSnapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_profilerSnapshot;
}

bool PipeWireGraph::setNodeVolume(uint32_t nodeId, float volume)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  volume = std::clamp(volume, 0.0f, 2.0f);

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  NodeBinding* binding = nullptr;
  PwNodeControls current{};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_nodeBindings.value(nodeId, nullptr);
    current = m_nodeControls.value(nodeId, PwNodeControls{});
  }

  if (!binding || !binding->node) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  uint8_t buffer[2048];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_pod_frame f;
  spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

  if (!current.channelVolumes.isEmpty()) {
    QVector<float> volumes(current.channelVolumes.size(), volume);
    spa_pod_builder_add(&b,
                        SPA_PROP_channelVolumes,
                        SPA_POD_Array(sizeof(float), SPA_TYPE_Float, volumes.size(), volumes.constData()),
                        0);
  } else {
    spa_pod_builder_add(&b, SPA_PROP_volume, SPA_POD_Float(volume), 0);
  }

  if (current.hasMute) {
    spa_pod_builder_add(&b, SPA_PROP_mute, SPA_POD_Bool(current.mute), 0);
  }

  const spa_pod* pod = reinterpret_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
  const int res = pw_node_set_param(binding->node, SPA_PARAM_Props, 0, pod);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setNodeMute(uint32_t nodeId, bool mute)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  NodeBinding* binding = nullptr;
  PwNodeControls current{};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_nodeBindings.value(nodeId, nullptr);
    current = m_nodeControls.value(nodeId, PwNodeControls{});
  }

  if (!binding || !binding->node) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  uint8_t buffer[2048];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_pod_frame f;
  spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

  spa_pod_builder_add(&b, SPA_PROP_mute, SPA_POD_Bool(mute), 0);
  if (current.hasVolume) {
    if (!current.channelVolumes.isEmpty()) {
      spa_pod_builder_add(&b,
                          SPA_PROP_channelVolumes,
                          SPA_POD_Array(sizeof(float), SPA_TYPE_Float, current.channelVolumes.size(), current.channelVolumes.constData()),
                          0);
    } else {
      spa_pod_builder_add(&b, SPA_PROP_volume, SPA_POD_Float(current.volume), 0);
    }
  }

  const spa_pod* pod = reinterpret_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
  const int res = pw_node_set_param(binding->node, SPA_PARAM_Props, 0, pod);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::createLink(uint32_t outputNodeId, uint32_t outputPortId, uint32_t inputNodeId, uint32_t inputPortId)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  if (!m_registry) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  pw_properties* props = pw_properties_new(nullptr, nullptr);
  pw_properties_set(props, PW_KEY_OBJECT_LINGER, "true");
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", outputNodeId);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", outputPortId);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", inputNodeId);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", inputPortId);

  void* proxy = pw_core_create_object(m_pw->core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
  pw_properties_free(props);

  if (proxy) {
    // Keep the proxy alive for the lifetime of this PipeWire connection.
    // This avoids destroying the server-side resource (the link). With
    // object.linger=true, links can outlive this process when we disconnect.
    m_createdLinkProxies.push_back(proxy);
  }

  pw_thread_loop_unlock(loop);
  return proxy != nullptr;
}

bool PipeWireGraph::destroyLink(uint32_t linkId)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  if (!m_registry) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const int res = pw_registry_destroy(m_registry, linkId);
  pw_thread_loop_unlock(loop);
  return res >= 0;
}

void PipeWireGraph::scheduleGraphChanged()
{
  bool expected = false;
  if (!m_emitScheduled.compare_exchange_strong(expected, true)) {
    return;
  }

  QMetaObject::invokeMethod(
      this,
      [this]() {
        m_emitScheduled.store(false);
        emit graphChanged();
      },
      Qt::QueuedConnection);
}

void PipeWireGraph::onRegistryGlobal(void* data,
                                     uint32_t id,
                                     uint32_t /*permissions*/,
                                     const char* type,
                                     uint32_t /*version*/,
                                     const struct spa_dict* props)
{
  auto* self = static_cast<PipeWireGraph*>(data);
  if (!type) {
    return;
  }

  const QString iface = QString::fromUtf8(type);

  bool changed = false;
  bool isNode = false;
  bool isMetadata = false;
  bool isProfiler = false;
  QString metadataName;
  {
    std::lock_guard<std::mutex> lock(self->m_mutex);

    if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Node)) {
      PwNodeInfo node;
      node.id = id;
      node.name = dictString(props, PW_KEY_NODE_NAME);
      node.description = dictString(props, PW_KEY_NODE_DESCRIPTION);
      node.mediaClass = dictString(props, PW_KEY_MEDIA_CLASS);
      node.appName = dictString(props, PW_KEY_APP_NAME);
      node.appProcessBinary = dictString(props, PW_KEY_APP_PROCESS_BINARY);
      node.objectSerial = dictString(props, PW_KEY_OBJECT_SERIAL);
      if (node.description.isEmpty()) {
        node.description = dictString(props, PW_KEY_NODE_NICK);
      }
      if (node.description.isEmpty()) {
        node.description = node.name;
      }
      self->m_nodes.insert(id, node);
      changed = true;
      isNode = true;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Port)) {
      PwPortInfo port;
      port.id = id;
      port.nodeId = dictU32(props, PW_KEY_NODE_ID).value_or(0);
      port.name = dictString(props, PW_KEY_PORT_NAME);
      port.alias = dictString(props, PW_KEY_PORT_ALIAS);
      port.direction = dictString(props, PW_KEY_PORT_DIRECTION);
      port.audioChannel = dictString(props, PW_KEY_AUDIO_CHANNEL);
      port.mediaType = dictString(props, PW_KEY_MEDIA_TYPE);
      port.formatDsp = dictString(props, PW_KEY_FORMAT_DSP);
      port.objectSerial = dictString(props, PW_KEY_OBJECT_SERIAL);
      if (port.name.isEmpty()) {
        port.name = !port.alias.isEmpty() ? port.alias : dictString(props, PW_KEY_OBJECT_PATH);
      }
      if (port.alias.isEmpty()) {
        port.alias = port.name;
      }
      self->m_ports.insert(id, port);
      changed = true;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Link)) {
      PwLinkInfo link;
      link.id = id;
      link.outputNodeId = dictU32(props, PW_KEY_LINK_OUTPUT_NODE).value_or(0);
      link.outputPortId = dictU32(props, PW_KEY_LINK_OUTPUT_PORT).value_or(0);
      link.inputNodeId = dictU32(props, PW_KEY_LINK_INPUT_NODE).value_or(0);
      link.inputPortId = dictU32(props, PW_KEY_LINK_INPUT_PORT).value_or(0);
      self->m_links.insert(id, link);
      changed = true;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Module)) {
      PwModuleInfo m;
      m.id = id;
      m.name = dictString(props, PW_KEY_MODULE_NAME);
      m.description = dictString(props, PW_KEY_MODULE_DESCRIPTION);
      m.objectSerial = dictString(props, PW_KEY_OBJECT_SERIAL);
      if (m.description.isEmpty()) {
        m.description = m.name;
      }
      self->m_modules.insert(id, m);
      changed = true;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Metadata)) {
      metadataName = dictString(props, PW_KEY_METADATA_NAME);
      if (metadataName == QStringLiteral("settings") || metadataName == QStringLiteral("default")) {
        changed = true;
        isMetadata = true;
      }
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Profiler)) {
      changed = true;
      isProfiler = true;
    }
  }

  if (isNode) {
    self->bindNode(id);
  }
  if (isMetadata) {
    self->bindMetadata(id, metadataName);
  }
  if (isProfiler) {
    self->bindProfiler(id);
  }

  if (changed) {
    self->scheduleGraphChanged();
  }
}

void PipeWireGraph::onRegistryGlobalRemove(void* data, uint32_t id)
{
  auto* self = static_cast<PipeWireGraph*>(data);

  bool changed = false;
  bool removedNode = false;
  bool removedMetadata = false;
  bool removedProfiler = false;
  {
    std::lock_guard<std::mutex> lock(self->m_mutex);
    if (self->m_links.remove(id) > 0) {
      changed = true;
    }
    if (self->m_ports.remove(id) > 0) {
      changed = true;
    }
    if (self->m_modules.remove(id) > 0) {
      changed = true;
    }
    if (self->m_nodes.remove(id) > 0) {
      changed = true;
      removedNode = true;
    }
    if (self->m_nodeControls.remove(id) > 0) {
      changed = true;
    }
    if (self->m_metadataBindings.contains(id)) {
      changed = true;
      removedMetadata = true;
    }
    if (self->m_profilerBinding && self->m_profilerBinding->profilerId == id) {
      changed = true;
      removedProfiler = true;
    }
  }

  if (removedNode) {
    self->unbindNode(id);
  }
  if (removedMetadata) {
    self->unbindMetadata(id);
  }
  if (removedProfiler) {
    self->unbindProfiler(id);
  }

  if (changed) {
    self->scheduleGraphChanged();
  }
}

void PipeWireGraph::onNodeInfo(void* data, const struct pw_node_info* /*info*/)
{
  auto* binding = static_cast<NodeBinding*>(data);
  if (!binding || !binding->graph) {
    return;
  }
}

static const struct spa_pod* resolveChoice(const struct spa_pod* pod)
{
  if (!pod) {
    return nullptr;
  }
  if (SPA_POD_TYPE(pod) != SPA_TYPE_Choice) {
    return pod;
  }

  uint32_t n = 0;
  uint32_t choice = 0;
  const spa_pod* child = spa_pod_get_values(pod, &n, &choice);
  if (!child || n == 0) {
    return nullptr;
  }
  return child;
}

void PipeWireGraph::onNodeParam(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/, const struct spa_pod* param)
{
  auto* binding = static_cast<NodeBinding*>(data);
  if (!binding || !binding->graph || !param) {
    return;
  }
  if (id != SPA_PARAM_Props) {
    return;
  }

  // Nodes may expose multiple Props params, and some of them don't include
  // volume/mute controls. Preserve any previously observed controls and only
  // update the fields present in this param so we don't accidentally clear
  // hasVolume/hasMute.
  PwNodeControls controls{};
  {
    std::lock_guard<std::mutex> lock(binding->graph->m_mutex);
    controls = binding->graph->m_nodeControls.value(binding->nodeId, PwNodeControls{});
  }

  if (const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute)) {
    const spa_pod* v = resolveChoice(&prop->value);
    bool mute = false;
    if (v && spa_pod_get_bool(v, &mute) == 0) {
      controls.hasMute = true;
      controls.mute = mute;
    }
  }

  if (const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes)) {
    const spa_pod* v = resolveChoice(&prop->value);
    if (v && spa_pod_is_array(v) && SPA_POD_ARRAY_VALUE_TYPE(v) == SPA_TYPE_Float) {
      uint32_t n = 0;
      const float* values = static_cast<const float*>(spa_pod_get_array(v, &n));
      if (values && n > 0) {
        controls.hasVolume = true;
        controls.channelVolumes.resize(static_cast<int>(n));
        for (uint32_t i = 0; i < n; ++i) {
          controls.channelVolumes[static_cast<int>(i)] = values[i];
        }
        float sum = 0.0f;
        for (float f : controls.channelVolumes) {
          sum += f;
        }
        controls.volume = sum / static_cast<float>(controls.channelVolumes.size());
      }
    }
  }

  if (const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_PROP_volume)) {
    // Only apply SPA_PROP_volume when this param does not include channelVolumes.
    if (!spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes)) {
      // Some nodes (e.g. ALSA sinks) expose multiple Props params. If we already saw channelVolumes,
      // ignore a scalar-only volume update so we don't accidentally clear channelVolumes and break
      // future volume writes that rely on channelVolumes.
      if (controls.channelVolumes.isEmpty()) {
        const spa_pod* v = resolveChoice(&prop->value);
        float volume = 1.0f;
        if (v && spa_pod_get_float(v, &volume) == 0) {
          controls.hasVolume = true;
          controls.volume = volume;
        }
      }
    }
  }

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(binding->graph->m_mutex);
    const auto it = binding->graph->m_nodeControls.constFind(binding->nodeId);
    if (it == binding->graph->m_nodeControls.constEnd() || it.value().mute != controls.mute || it.value().volume != controls.volume ||
        it.value().channelVolumes != controls.channelVolumes || it.value().hasMute != controls.hasMute || it.value().hasVolume != controls.hasVolume) {
      binding->graph->m_nodeControls.insert(binding->nodeId, controls);
      changed = true;
    }
  }

  if (changed) {
    binding->graph->scheduleGraphChanged();
  }
}

int PipeWireGraph::onMetadataProperty(void* data, uint32_t subject, const char* key, const char* /*type*/, const char* value)
{
  auto* binding = static_cast<MetadataBinding*>(data);
  if (!binding || !binding->graph || !key) {
    return 0;
  }

  PipeWireGraph* graph = binding->graph;
  if (subject != 0) {
    return 0;
  }

  const QString k = QString::fromUtf8(key);
  auto parseU32 = [](const char* s) -> std::optional<uint32_t> {
    if (!s || s[0] == '\0') {
      return std::nullopt;
    }
    bool ok = false;
    const uint32_t v = QString::fromUtf8(s).toUInt(&ok);
    return ok ? std::optional<uint32_t>(v) : std::nullopt;
  };

  auto parseAllowedRates = [](const QString& raw) -> QVector<uint32_t> {
    QString s = raw;
    s.remove('[');
    s.remove(']');
    const QStringList parts = s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QVector<uint32_t> out;
    out.reserve(parts.size());
    for (const auto& p : parts) {
      bool ok = false;
      const uint32_t v = p.toUInt(&ok);
      if (ok) {
        out.push_back(v);
      }
    }
    return out;
  };

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(graph->m_mutex);
    auto update = [&](std::optional<uint32_t>& slot) {
      const std::optional<uint32_t> parsed = parseU32(value);
      if (slot != parsed) {
        slot = parsed;
        changed = true;
      }
    };

    if (k == QStringLiteral("default.audio.sink")) {
      update(graph->m_defaultAudioSinkId);
    } else if (k == QStringLiteral("default.configured.audio.sink")) {
      update(graph->m_configuredAudioSinkId);
    } else if (k == QStringLiteral("default.audio.source")) {
      update(graph->m_defaultAudioSourceId);
    } else if (k == QStringLiteral("default.configured.audio.source")) {
      update(graph->m_configuredAudioSourceId);
    } else if (binding->name == QStringLiteral("settings")) {
      if (k == QStringLiteral("clock.rate")) {
        update(graph->m_clockRate);
      } else if (k == QStringLiteral("clock.allowed-rates")) {
        const QString raw = QString::fromUtf8(value ? value : "");
        const QVector<uint32_t> parsed = parseAllowedRates(raw);
        if (graph->m_clockAllowedRates != parsed) {
          graph->m_clockAllowedRates = parsed;
          changed = true;
        }
      } else if (k == QStringLiteral("clock.quantum")) {
        update(graph->m_clockQuantum);
      } else if (k == QStringLiteral("clock.min-quantum")) {
        update(graph->m_clockMinQuantum);
      } else if (k == QStringLiteral("clock.max-quantum")) {
        update(graph->m_clockMaxQuantum);
      } else if (k == QStringLiteral("clock.force-rate")) {
        const std::optional<uint32_t> v = parseU32(value);
        const std::optional<uint32_t> parsed = (v.has_value() && *v > 0) ? v : std::nullopt;
        if (graph->m_clockForceRate != parsed) {
          graph->m_clockForceRate = parsed;
          changed = true;
        }
      } else if (k == QStringLiteral("clock.force-quantum")) {
        const std::optional<uint32_t> v = parseU32(value);
        const std::optional<uint32_t> parsed = (v.has_value() && *v > 0) ? v : std::nullopt;
        if (graph->m_clockForceQuantum != parsed) {
          graph->m_clockForceQuantum = parsed;
          changed = true;
        }
      }
    }
  }

  if (changed) {
    graph->scheduleGraphChanged();
  }

  return 0;
}

static QVector<const struct spa_pod*> collectStructItems(const struct spa_pod* pod)
{
  QVector<const struct spa_pod*> items;
  if (!pod || SPA_POD_TYPE(pod) != SPA_TYPE_Struct) {
    return items;
  }
  const auto* s = reinterpret_cast<const struct spa_pod_struct*>(pod);
  const void* it = nullptr;
  SPA_POD_STRUCT_FOREACH(s, it)
  {
    if (it) {
      items.push_back(reinterpret_cast<const struct spa_pod*>(it));
    }
  }
  return items;
}

static std::optional<double> fractionToMs(const struct spa_fraction& f)
{
  if (f.denom == 0) {
    return std::nullopt;
  }
  return (static_cast<double>(f.num) * 1000.0) / static_cast<double>(f.denom);
}

static void parseProfilerInfo(const struct spa_pod* pod, PwProfilerSnapshot& out)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);

  auto parseFlat = [&](const QVector<const struct spa_pod*>& e, int offset, bool withXruns) -> bool {
    if (e.size() < offset + (withXruns ? 5 : 4)) {
      return false;
    }
    int64_t counter = 0;
    float fast = 0.0f;
    float medium = 0.0f;
    float slow = 0.0f;
    int32_t xruns = 0;

    if (spa_pod_get_long(e[offset + 0], &counter) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 1], &fast) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 2], &medium) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 3], &slow) != 0) {
      return false;
    }
    if (withXruns) {
      if (spa_pod_get_int(e[offset + 4], &xruns) != 0) {
        return false;
      }
    }

    out.hasInfo = true;
    out.counter = static_cast<uint64_t>(std::max<int64_t>(0, counter));
    out.cpuLoadFast = static_cast<double>(fast);
    out.cpuLoadMedium = static_cast<double>(medium);
    out.cpuLoadSlow = static_cast<double>(slow);
    if (withXruns) {
      out.xrunCount = static_cast<int>(xruns);
    }
    return true;
  };

  // Most common: Struct(Long counter, Float fast, Float medium, Float slow, Int xrunCount)
  if (parseFlat(items, 0, true)) {
    return;
  }

  // Some versions may nest the cpu loads in a Struct and then append xrun count.
  // Struct(Struct(Long, Float, Float, Float), Int xrunCount)
  if (items.size() >= 2 && SPA_POD_TYPE(items[0]) == SPA_TYPE_Struct) {
    int32_t xruns = 0;
    if (spa_pod_get_int(items[1], &xruns) == 0) {
      const QVector<const struct spa_pod*> nested = collectStructItems(items[0]);
      if (parseFlat(nested, 0, false)) {
        out.xrunCount = static_cast<int>(xruns);
        return;
      }
    }
  }
}

static void parseProfilerClock(const struct spa_pod* pod, PwProfilerSnapshot& out)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);
  if (items.size() < 13) {
    return;
  }

  int32_t cycle = 0;
  int64_t duration = 0;
  int64_t delay = 0;
  int64_t xrunDuration = 0;

  // indices per spa/param/profiler.h
  const bool okDuration = (spa_pod_get_long(items[6], &duration) == 0);
  const bool okDelay = (spa_pod_get_long(items[7], &delay) == 0);
  const bool okCycle = (spa_pod_get_int(items[11], &cycle) == 0);
  const bool okXrun = (spa_pod_get_long(items[12], &xrunDuration) == 0);

  out.hasClock = okDuration || okDelay || okCycle || okXrun;
  if (okDuration) {
    out.clockDurationMs = static_cast<double>(duration) / 1'000'000.0;
  }
  if (okDelay) {
    out.clockDelayMs = static_cast<double>(delay) / 1'000'000.0;
  }
  if (okXrun) {
    out.clockXrunDurationMs = static_cast<double>(xrunDuration) / 1'000'000.0;
  }
  if (okCycle) {
    out.clockCycle = static_cast<int>(cycle);
  }
}

static std::optional<PwProfilerBlock> parseProfilerBlock(const struct spa_pod* pod)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);
  if (items.size() < 9) {
    return std::nullopt;
  }

  int32_t id = 0;
  const char* name = nullptr;
  int64_t prevSignal = 0;
  int64_t signal = 0;
  int64_t awake = 0;
  int64_t finish = 0;
  int32_t status = 0;
  struct spa_fraction latency{};
  int32_t xruns = 0;

  if (spa_pod_get_int(items[0], &id) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_string(items[1], &name) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[2], &prevSignal) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[3], &signal) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[4], &awake) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[5], &finish) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_int(items[6], &status) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_fraction(items[7], &latency) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_int(items[8], &xruns) != 0) {
    return std::nullopt;
  }

  PwProfilerBlock b;
  b.id = static_cast<uint32_t>(std::max(0, id));
  b.name = QString::fromUtf8(name ? name : "");
  b.status = static_cast<int>(status);
  b.xrunCount = static_cast<int>(xruns);
  b.latencyMs = fractionToMs(latency);

  const int64_t periodNs = signal - prevSignal;
  const int64_t waitNs = awake - signal;
  const int64_t busyNs = finish - awake;

  if (periodNs > 0) {
    if (waitNs >= 0) {
      b.waitMs = static_cast<double>(waitNs) / 1'000'000.0;
      b.waitRatio = static_cast<double>(waitNs) / static_cast<double>(periodNs);
    }
    if (busyNs >= 0) {
      b.busyMs = static_cast<double>(busyNs) / 1'000'000.0;
      b.busyRatio = static_cast<double>(busyNs) / static_cast<double>(periodNs);
    }
  }

  return b;
}

void PipeWireGraph::onProfilerProfile(void* data, const struct spa_pod* pod)
{
  auto* binding = static_cast<ProfilerBinding*>(data);
  if (!binding || !binding->graph || !pod) {
    return;
  }

  if (SPA_POD_TYPE(pod) != SPA_TYPE_Object) {
    return;
  }

  PwProfilerSnapshot snap;

  const auto* obj = reinterpret_cast<const struct spa_pod_object*>(pod);
  const struct spa_pod_prop* prop = nullptr;
  SPA_POD_OBJECT_FOREACH(obj, prop)
  {
    if (!prop) {
      continue;
    }
    const struct spa_pod* v = &prop->value;
    switch (prop->key) {
    case SPA_PROFILER_info:
      parseProfilerInfo(v, snap);
      break;
    case SPA_PROFILER_clock:
      parseProfilerClock(v, snap);
      break;
    case SPA_PROFILER_driverBlock: {
      const auto b = parseProfilerBlock(v);
      if (b.has_value()) {
        snap.drivers.push_back(*b);
      }
      break;
    }
    case SPA_PROFILER_followerBlock: {
      const auto b = parseProfilerBlock(v);
      if (b.has_value()) {
        snap.followers.push_back(*b);
      }
      break;
    }
    default:
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(binding->graph->m_mutex);
    const uint64_t nextSeq = binding->graph->m_profilerSnapshot.has_value() ? (binding->graph->m_profilerSnapshot->seq + 1) : 1;
    snap.seq = nextSeq;
    binding->graph->m_profilerSnapshot = snap;
  }
}

void PipeWireGraph::bindNode(uint32_t id)
{
  if (!m_registry) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_nodeBindings.contains(id)) {
      return;
    }
  }

  auto* binding = new NodeBinding();
  binding->graph = this;
  binding->nodeId = id;
  binding->node = reinterpret_cast<pw_node*>(pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));

  if (!binding->node) {
    delete binding;
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_nodeBindings.contains(id)) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->node));
      delete binding;
      return;
    }
    m_nodeBindings.insert(id, binding);
  }

  static const pw_node_events nodeEvents = [] {
    pw_node_events e{};
    e.version = PW_VERSION_NODE_EVENTS;
    e.info = &PipeWireGraph::onNodeInfo;
    e.param = &PipeWireGraph::onNodeParam;
    return e;
  }();
  pw_node_add_listener(binding->node, &binding->listener, &nodeEvents, binding);

  uint32_t ids[] = {SPA_PARAM_Props};
  pw_node_subscribe_params(binding->node, ids, 1);
  pw_node_enum_params(binding->node, 0, SPA_PARAM_Props, 0, 1, nullptr);
}

void PipeWireGraph::unbindNode(uint32_t id)
{
  NodeBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_nodeBindings.take(id);
  }

  if (!binding) {
    return;
  }

  spa_hook_remove(&binding->listener);
  if (binding->node) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->node));
  }
  delete binding;
}

void PipeWireGraph::bindMetadata(uint32_t id, const QString& name)
{
  if (!m_registry || name.isEmpty()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_metadataBindings.contains(id)) {
      return;
    }
  }

  auto* binding = new MetadataBinding();
  binding->graph = this;
  binding->metadataId = id;
  binding->name = name;
  binding->metadata = reinterpret_cast<pw_metadata*>(pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));

  if (!binding->metadata) {
    delete binding;
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_metadataBindings.contains(id)) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->metadata));
      delete binding;
      return;
    }
    m_metadataBindings.insert(id, binding);
    m_settingsMetadata = nullptr;
    m_defaultDeviceMetadata = nullptr;
    for (auto* b : m_metadataBindings) {
      if (!b) {
        continue;
      }
      if (b->name == QStringLiteral("settings")) {
        m_settingsMetadata = b;
      }
      if (b->name == QStringLiteral("default")) {
        m_defaultDeviceMetadata = b;
      }
    }
    if (!m_defaultDeviceMetadata) {
      m_defaultDeviceMetadata = m_settingsMetadata;
    }
  }

  static const pw_metadata_events metadataEvents = [] {
    pw_metadata_events e{};
    e.version = PW_VERSION_METADATA_EVENTS;
    e.property = &PipeWireGraph::onMetadataProperty;
    return e;
  }();
  pw_metadata_add_listener(binding->metadata, &binding->listener, &metadataEvents, binding);
}

void PipeWireGraph::unbindMetadata(uint32_t id)
{
  MetadataBinding* binding = nullptr;
  bool removedDefaultDevice = false;
  bool removedSettings = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    removedDefaultDevice = (m_defaultDeviceMetadata && m_defaultDeviceMetadata->metadataId == id);
    removedSettings = (m_settingsMetadata && m_settingsMetadata->metadataId == id);
    binding = m_metadataBindings.take(id);

    if (removedDefaultDevice) {
      m_defaultAudioSinkId.reset();
      m_configuredAudioSinkId.reset();
      m_defaultAudioSourceId.reset();
      m_configuredAudioSourceId.reset();
    }
    if (removedSettings) {
      m_clockRate.reset();
      m_clockAllowedRates.clear();
      m_clockQuantum.reset();
      m_clockMinQuantum.reset();
      m_clockMaxQuantum.reset();
      m_clockForceRate.reset();
      m_clockForceQuantum.reset();
    }

    m_settingsMetadata = nullptr;
    m_defaultDeviceMetadata = nullptr;
    for (auto* b : m_metadataBindings) {
      if (!b) {
        continue;
      }
      if (b->name == QStringLiteral("settings")) {
        m_settingsMetadata = b;
      }
      if (b->name == QStringLiteral("default")) {
        m_defaultDeviceMetadata = b;
      }
    }
    if (!m_defaultDeviceMetadata) {
      m_defaultDeviceMetadata = m_settingsMetadata;
    }
  }

  if (!binding) {
    return;
  }

  spa_hook_remove(&binding->listener);
  if (binding->metadata) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->metadata));
  }
  delete binding;
}

void PipeWireGraph::bindProfiler(uint32_t id)
{
  if (!m_registry) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profilerBinding) {
      return;
    }
  }

  auto* binding = new ProfilerBinding();
  binding->graph = this;
  binding->profilerId = id;
  binding->profiler = reinterpret_cast<pw_profiler*>(pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Profiler, PW_VERSION_PROFILER, 0));

  if (!binding->profiler) {
    delete binding;
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profilerBinding) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->profiler));
      delete binding;
      return;
    }
    m_profilerBinding = binding;
  }

  static const pw_profiler_events profilerEvents = [] {
    pw_profiler_events e{};
    e.version = PW_VERSION_PROFILER_EVENTS;
    e.profile = &PipeWireGraph::onProfilerProfile;
    return e;
  }();
  pw_profiler_add_listener(binding->profiler, &binding->listener, &profilerEvents, binding);
}

void PipeWireGraph::unbindProfiler(uint32_t id)
{
  ProfilerBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_profilerBinding || m_profilerBinding->profilerId != id) {
      return;
    }
    binding = m_profilerBinding;
    m_profilerBinding = nullptr;
    m_profilerSnapshot.reset();
  }

  spa_hook_remove(&binding->listener);
  if (binding->profiler) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->profiler));
  }
  delete binding;
}
