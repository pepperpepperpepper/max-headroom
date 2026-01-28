#include "PipeWireGraphInternal.h"

#include <pipewire/core.h>
#include <pipewire/node.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <algorithm>

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
  PwNodeInfo nodeInfo{};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_nodeBindings.value(nodeId, nullptr);
    current = m_nodeControls.value(nodeId, PwNodeControls{});
    nodeInfo = m_nodes.value(nodeId, PwNodeInfo{});
  }

  if (!binding || !binding->node) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  auto trySet = [&](bool useChannelVolumes) -> int {
    uint8_t buffer[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

    if (useChannelVolumes) {
      int channels = current.channelVolumes.size();
      if (channels <= 0) {
        channels = static_cast<int>(nodeInfo.audioChannels);
      }
      if (channels <= 0) {
        channels = 2;
      }
      QVector<float> volumes(channels, volume);
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
    return pw_node_set_param(binding->node, SPA_PARAM_Props, 0, pod);
  };

  int res = trySet(true);
  if (res < 0) {
    res = trySet(false);
  }

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

