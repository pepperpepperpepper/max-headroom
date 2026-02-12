#include "PipeWireGraphInternal.h"
#include "DebugEnv.h"
#include "VolumeScale.h"

#include <QDebug>

#include <pipewire/core.h>
#include <pipewire/node.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <algorithm>

namespace {
QString nodeLabel(const PwNodeInfo& n)
{
  return n.description.isEmpty() ? n.name : n.description;
}
} // namespace

bool PipeWireGraph::setNodeVolume(uint32_t nodeId, float volume)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  const float requestedVolume = volume;
  volume = std::clamp(volume, 0.0f, headroom::volume::kUiMaxLinear);

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

  const bool debugOps =
      headroom::debug::pwOpsEnabled() && headroom::debug::pwNodeFilterMatches(nodeId, nodeInfo.name, nodeInfo.description);
  auto logOp = [&](const QString& msg) {
    qDebug().noquote() << QStringLiteral("headroom: debug: %1").arg(msg);
  };

  if (!binding || !binding->node) {
    if (debugOps) {
      logOp(QStringLiteral("setNodeVolume nodeId=%1 node=%2 (missing binding)").arg(nodeId).arg(nodeLabel(nodeInfo)));
    }
    pw_thread_loop_unlock(loop);
    return false;
  }

  if (debugOps) {
    logOp(QStringLiteral("setNodeVolume nodeId=%1 node=%2 requestedLinear=%3 (ui=%4%%) clampedLinear=%5 (ui=%6%%) hasMute=%7 mute=%8")
              .arg(nodeId)
              .arg(nodeLabel(nodeInfo))
              .arg(QString::number(requestedVolume, 'f', 4))
              .arg(headroom::volume::linearToUiPercent(requestedVolume))
              .arg(QString::number(volume, 'f', 4))
              .arg(headroom::volume::linearToUiPercent(volume))
              .arg(current.hasMute ? 1 : 0)
              .arg(current.mute ? 1 : 0));
  }

  auto setWithChannelVolumes = [&]() -> int {
    uint8_t buffer[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

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

    if (current.hasMute) {
      spa_pod_builder_add(&b, SPA_PROP_mute, SPA_POD_Bool(current.mute), 0);
    }

    const spa_pod* pod = reinterpret_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
    const int res = pw_node_set_param(binding->node, SPA_PARAM_Props, 0, pod);
    if (debugOps) {
      logOp(QStringLiteral("setNodeVolume nodeId=%1 write=channelVolumes channels=%2 value=%3 res=%4")
                .arg(nodeId)
                .arg(channels)
                .arg(QString::number(volume, 'f', 4))
                .arg(res));
    }
    return res;
  };

  auto setWithScalarVolume = [&]() -> int {
    uint8_t buffer[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame f;
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

    spa_pod_builder_add(&b, SPA_PROP_volume, SPA_POD_Float(volume), 0);
    if (current.hasMute) {
      spa_pod_builder_add(&b, SPA_PROP_mute, SPA_POD_Bool(current.mute), 0);
    }

    const spa_pod* pod = reinterpret_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
    const int res = pw_node_set_param(binding->node, SPA_PARAM_Props, 0, pod);
    if (debugOps) {
      logOp(QStringLiteral("setNodeVolume nodeId=%1 write=volume value=%2 res=%3").arg(nodeId).arg(QString::number(volume, 'f', 4)).arg(res));
    }
    return res;
  };

  int res = setWithChannelVolumes();
  if (res < 0) {
    res = setWithScalarVolume();
  }

  if (debugOps) {
    logOp(QStringLiteral("setNodeVolume nodeId=%1 done ok=%2").arg(nodeId).arg(res >= 0 ? 1 : 0));
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
  PwNodeInfo nodeInfo{};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_nodeBindings.value(nodeId, nullptr);
    current = m_nodeControls.value(nodeId, PwNodeControls{});
    nodeInfo = m_nodes.value(nodeId, PwNodeInfo{});
  }

  const bool debugOps =
      headroom::debug::pwOpsEnabled() && headroom::debug::pwNodeFilterMatches(nodeId, nodeInfo.name, nodeInfo.description);
  auto logOp = [&](const QString& msg) {
    qDebug().noquote() << QStringLiteral("headroom: debug: %1").arg(msg);
  };

  if (!binding || !binding->node) {
    if (debugOps) {
      logOp(QStringLiteral("setNodeMute nodeId=%1 node=%2 (missing binding)").arg(nodeId).arg(nodeLabel(nodeInfo)));
    }
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

  if (debugOps) {
    logOp(QStringLiteral("setNodeMute nodeId=%1 node=%2 mute=%3 res=%4").arg(nodeId).arg(nodeLabel(nodeInfo)).arg(mute ? 1 : 0).arg(res));
  }

  pw_thread_loop_unlock(loop);
  return res >= 0;
}
