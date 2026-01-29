#include "PipeWireGraphInternal.h"

#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/extensions/profiler.h>
#include <pipewire/link.h>
#include <pipewire/module.h>
#include <pipewire/node.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include <algorithm>

using namespace pipewiregraph_internal;

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

  uint32_t changeFlags = 0;
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
      node.audioChannels = dictU32(props, PW_KEY_AUDIO_CHANNELS).value_or(0);
      node.audioPosition = dictString(props, "audio.position");
      if (node.description.isEmpty()) {
        node.description = dictString(props, PW_KEY_NODE_NICK);
      }
      if (node.description.isEmpty()) {
        node.description = node.name;
      }
      self->m_nodes.insert(id, node);
      changeFlags |= ChangeTopology;
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
      changeFlags |= ChangeTopology;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Link)) {
      PwLinkInfo link;
      link.id = id;
      link.outputNodeId = dictU32(props, PW_KEY_LINK_OUTPUT_NODE).value_or(0);
      link.outputPortId = dictU32(props, PW_KEY_LINK_OUTPUT_PORT).value_or(0);
      link.inputNodeId = dictU32(props, PW_KEY_LINK_INPUT_NODE).value_or(0);
      link.inputPortId = dictU32(props, PW_KEY_LINK_INPUT_PORT).value_or(0);
      self->m_links.insert(id, link);
      changeFlags |= ChangeTopology;
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
      changeFlags |= ChangeTopology;
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Metadata)) {
      metadataName = dictString(props, PW_KEY_METADATA_NAME);
      if (metadataName == QStringLiteral("settings") || metadataName == QStringLiteral("default")) {
        changeFlags |= ChangeMetadata;
        isMetadata = true;
      }
    } else if (iface == QString::fromUtf8(PW_TYPE_INTERFACE_Profiler)) {
      changeFlags |= ChangeTopology;
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

  if (changeFlags != 0) {
    self->scheduleGraphChanged(changeFlags);
  }
}

void PipeWireGraph::onRegistryGlobalRemove(void* data, uint32_t id)
{
  auto* self = static_cast<PipeWireGraph*>(data);

  uint32_t changeFlags = 0;
  bool removedNode = false;
  bool removedMetadata = false;
  bool removedProfiler = false;
  {
    std::lock_guard<std::mutex> lock(self->m_mutex);
    if (self->m_links.remove(id) > 0) {
      changeFlags |= ChangeTopology;
    }
    if (self->m_ports.remove(id) > 0) {
      changeFlags |= ChangeTopology;
    }
    if (self->m_modules.remove(id) > 0) {
      changeFlags |= ChangeTopology;
    }
    if (self->m_nodes.remove(id) > 0) {
      changeFlags |= ChangeTopology;
      removedNode = true;
    }
    if (self->m_nodeControls.remove(id) > 0) {
      changeFlags |= ChangeTopology;
    }
    if (self->m_metadataBindings.contains(id)) {
      changeFlags |= ChangeMetadata;
      removedMetadata = true;
    }
    if (self->m_profilerBinding && self->m_profilerBinding->profilerId == id) {
      changeFlags |= ChangeTopology;
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

  if (changeFlags != 0) {
    self->scheduleGraphChanged(changeFlags);
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
      const spa_pod* v = resolveChoice(&prop->value);
      float volume = 1.0f;
      if (v && spa_pod_get_float(v, &volume) == 0) {
        controls.hasVolume = true;

        // Some nodes emit scalar-only volume updates even when channelVolumes is supported.
        // Preserve channelVolumes and rescale it to match the new scalar volume so:
        // - UI reflects the updated volume
        // - future volume writes can keep using channelVolumes safely
        const float old = controls.volume;
        controls.volume = volume;
        if (!controls.channelVolumes.isEmpty()) {
          if (old > 0.000001f) {
            const float ratio = volume / old;
            for (float& cv : controls.channelVolumes) {
              cv = std::clamp(cv * ratio, 0.0f, 2.0f);
            }
          } else {
            for (float& cv : controls.channelVolumes) {
              cv = std::clamp(volume, 0.0f, 2.0f);
            }
          }
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
    binding->graph->scheduleGraphChanged(ChangeNodeControls);
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
