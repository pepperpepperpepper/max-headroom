#include "tui/TuiInternal.h"

#include <QSet>

#include <algorithm>

namespace headroomtui {

Page pageFromIndex(int idx)
{
  idx = (idx % 8 + 8) % 8;
  return static_cast<Page>(idx);
}

int clampIndex(int idx, int size)
{
  if (size <= 0) {
    return 0;
  }
  return std::clamp(idx, 0, size - 1);
}

float clampVolume(float v)
{
  return std::clamp(v, kMinVolume, kMaxVolume);
}

QList<PwPortInfo> portsForNode(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction)
{
  QList<PwPortInfo> out;
  for (const auto& p : ports) {
    if (p.nodeId == nodeId && p.direction == direction) {
      out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwPortInfo& a, const PwPortInfo& b) { return a.id < b.id; });
  return out;
}

QList<PwNodeInfo> nodesWithPortDirection(const QList<PwNodeInfo>& nodes, const QList<PwPortInfo>& ports, const QString& direction)
{
  QSet<uint32_t> allowed;
  allowed.reserve(ports.size());
  for (const auto& p : ports) {
    if (p.direction == direction && p.nodeId != 0) {
      allowed.insert(p.nodeId);
    }
  }

  QList<PwNodeInfo> out;
  out.reserve(nodes.size());
  for (const auto& n : nodes) {
    if (allowed.contains(n.id)) {
      out.push_back(n);
    }
  }

  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    if (a.mediaClass != b.mediaClass) {
      return a.mediaClass < b.mediaClass;
    }
    if (a.description != b.description) {
      return a.description < b.description;
    }
    return a.name < b.name;
  });

  return out;
}

bool movePlaybackStreamToSink(PipeWireGraph* graph, uint32_t streamId, uint32_t sinkId)
{
  if (!graph || streamId == 0 || sinkId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamOutPorts = portsForNode(ports, streamId, QStringLiteral("out"));
  const QList<PwPortInfo> sinkInPorts = portsForNode(ports, sinkId, QStringLiteral("in"));
  if (streamOutPorts.isEmpty() || sinkInPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamOutPortIds;
  streamOutPortIds.reserve(streamOutPorts.size());
  for (const auto& p : streamOutPorts) {
    streamOutPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.outputNodeId == streamId && streamOutPortIds.contains(l.outputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sinkByChannel;
  sinkByChannel.reserve(sinkInPorts.size());
  for (const auto& p : sinkInPorts) {
    if (!p.audioChannel.isEmpty()) {
      sinkByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamOutPorts.size(); ++i) {
    const auto& sp = streamOutPorts[i];
    uint32_t targetPortId = 0;

    if (!sp.audioChannel.isEmpty() && sinkByChannel.contains(sp.audioChannel)) {
      targetPortId = sinkByChannel.value(sp.audioChannel);
    } else if (i < sinkInPorts.size()) {
      targetPortId = sinkInPorts[i].id;
    }

    if (targetPortId != 0) {
      anyOk = graph->createLink(streamId, sp.id, sinkId, targetPortId) || anyOk;
    }
  }

  return anyOk;
}

bool moveCaptureStreamToSource(PipeWireGraph* graph, uint32_t streamId, uint32_t sourceId)
{
  if (!graph || streamId == 0 || sourceId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamInPorts = portsForNode(ports, streamId, QStringLiteral("in"));
  const QList<PwPortInfo> sourceOutPorts = portsForNode(ports, sourceId, QStringLiteral("out"));
  if (streamInPorts.isEmpty() || sourceOutPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamInPortIds;
  streamInPortIds.reserve(streamInPorts.size());
  for (const auto& p : streamInPorts) {
    streamInPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.inputNodeId == streamId && streamInPortIds.contains(l.inputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sourceByChannel;
  sourceByChannel.reserve(sourceOutPorts.size());
  for (const auto& p : sourceOutPorts) {
    if (!p.audioChannel.isEmpty()) {
      sourceByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamInPorts.size(); ++i) {
    const auto& sp = streamInPorts[i];
    uint32_t sourcePortId = 0;

    if (!sp.audioChannel.isEmpty() && sourceByChannel.contains(sp.audioChannel)) {
      sourcePortId = sourceByChannel.value(sp.audioChannel);
    } else if (i < sourceOutPorts.size()) {
      sourcePortId = sourceOutPorts[i].id;
    }

    if (sourcePortId != 0) {
      anyOk = graph->createLink(sourceId, sourcePortId, streamId, sp.id) || anyOk;
    }
  }

  return anyOk;
}

} // namespace headroomtui
