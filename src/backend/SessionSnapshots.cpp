#include "SessionSnapshots.h"

#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"

#include <QJsonDocument>
#include <QSettings>

#include <algorithm>

namespace {
bool isInternalNodeName(const QString& nodeName)
{
  return nodeName.startsWith(QStringLiteral("headroom.meter.")) || nodeName == QStringLiteral("headroom.visualizer") ||
      nodeName == QStringLiteral("headroom.recorder");
}

bool isEqNodeName(const QString& nodeName)
{
  return nodeName.startsWith(QStringLiteral("headroom.eq."));
}

QString eqPresetKeyForNodeName(const QString& nodeName)
{
  return QStringLiteral("eq/%1/presetJson").arg(nodeName);
}
} // namespace

SessionSnapshot snapshotSession(const QString& snapshotName, const PipeWireGraph& graph, QSettings& s)
{
  SessionSnapshot snap;
  snap.name = snapshotName.trimmed();

  // Patchbay links.
  // If Headroom EQ is enabled, links may be rerouted through headroom.eq.* nodes; for session snapshots we
  // compress those links back to their logical endpoints so restoring the session works whether or not EQ
  // is currently active.
  {
    const QList<PwNodeInfo> nodes = graph.nodes();
    const QList<PwPortInfo> ports = graph.ports();
    const QList<PwLinkInfo> links = graph.links();

    QHash<uint32_t, QString> nodeNameById;
    nodeNameById.reserve(nodes.size());
    QHash<uint32_t, QString> mediaClassByNodeId;
    mediaClassByNodeId.reserve(nodes.size());
    QSet<uint32_t> eqNodeIds;
    eqNodeIds.reserve(nodes.size());

    for (const auto& n : nodes) {
      nodeNameById.insert(n.id, n.name);
      mediaClassByNodeId.insert(n.id, n.mediaClass);
      if (isEqNodeName(n.name)) {
        eqNodeIds.insert(n.id);
      }
    }

    QHash<uint32_t, PwPortInfo> portById;
    portById.reserve(ports.size());
    for (const auto& p : ports) {
      portById.insert(p.id, p);
    }

    auto channelKey = [](const PwPortInfo& p) {
      if (!p.audioChannel.trimmed().isEmpty()) {
        return p.audioChannel.trimmed().toUpper();
      }
      return p.name.trimmed().toUpper();
    };

    struct EqTarget final {
      uint32_t sinkTargetId = 0;
      uint32_t sourceTargetId = 0;
    };
    QHash<uint32_t, EqTarget> eqTargetByEqId;
    eqTargetByEqId.reserve(eqNodeIds.size());
    for (const uint32_t eqId : eqNodeIds) {
      eqTargetByEqId.insert(eqId, EqTarget{});
    }

    // Identify EQ targets by their wiring.
    for (const auto& l : links) {
      if (eqNodeIds.contains(l.outputNodeId)) {
        const QString inClass = mediaClassByNodeId.value(l.inputNodeId);
        if (inClass == QStringLiteral("Audio/Sink")) {
          eqTargetByEqId[l.outputNodeId].sinkTargetId = l.inputNodeId;
        }
      }
      if (eqNodeIds.contains(l.inputNodeId)) {
        const QString outClass = mediaClassByNodeId.value(l.outputNodeId);
        if (outClass == QStringLiteral("Audio/Source")) {
          eqTargetByEqId[l.inputNodeId].sourceTargetId = l.outputNodeId;
        }
      }
    }

    QHash<uint32_t, QHash<QString, PwPortInfo>> inPortsByNodeChannel;
    QHash<uint32_t, QHash<QString, PwPortInfo>> outPortsByNodeChannel;
    QHash<uint32_t, PwPortInfo> firstInPortByNode;
    QHash<uint32_t, PwPortInfo> firstOutPortByNode;
    inPortsByNodeChannel.reserve(nodes.size());
    outPortsByNodeChannel.reserve(nodes.size());
    firstInPortByNode.reserve(nodes.size());
    firstOutPortByNode.reserve(nodes.size());

    for (const auto& p : ports) {
      const QString ch = channelKey(p);
      if (p.direction == QStringLiteral("in")) {
        if (!inPortsByNodeChannel.contains(p.nodeId)) {
          inPortsByNodeChannel.insert(p.nodeId, {});
        }
        inPortsByNodeChannel[p.nodeId].insert(ch, p);
        if (!firstInPortByNode.contains(p.nodeId)) {
          firstInPortByNode.insert(p.nodeId, p);
        }
      } else if (p.direction == QStringLiteral("out")) {
        if (!outPortsByNodeChannel.contains(p.nodeId)) {
          outPortsByNodeChannel.insert(p.nodeId, {});
        }
        outPortsByNodeChannel[p.nodeId].insert(ch, p);
        if (!firstOutPortByNode.contains(p.nodeId)) {
          firstOutPortByNode.insert(p.nodeId, p);
        }
      }
    }

    auto portNameOrId = [&](uint32_t portId) {
      const auto it = portById.find(portId);
      if (it == portById.end() || it->name.trimmed().isEmpty()) {
        return QString::number(portId);
      }
      return it->name;
    };

    QSet<QString> seen;
    seen.reserve(links.size());

    auto addLink = [&](const QString& outNode, const QString& outPort, const QString& inNode, const QString& inPort) {
      if (outNode.trimmed().isEmpty() || outPort.trimmed().isEmpty() || inNode.trimmed().isEmpty() || inPort.trimmed().isEmpty()) {
        return;
      }
      if (isInternalNodeName(outNode) || isInternalNodeName(inNode)) {
        return;
      }
      PatchbayLinkSpec spec;
      spec.outputNodeName = outNode;
      spec.outputPortName = outPort;
      spec.inputNodeName = inNode;
      spec.inputPortName = inPort;
      const QString key = QStringLiteral("%1:%2 -> %3:%4").arg(outNode, outPort, inNode, inPort);
      if (seen.contains(key)) {
        return;
      }
      seen.insert(key);
      snap.links.push_back(spec);
    };

    for (const auto& l : links) {
      const QString outNodeName = nodeNameById.value(l.outputNodeId);
      const QString inNodeName = nodeNameById.value(l.inputNodeId);
      const QString outPortName = portNameOrId(l.outputPortId);
      const QString inPortName = portNameOrId(l.inputPortId);

      // Compress sink EQ: <X> -> eq  becomes <X> -> sink
      if (eqNodeIds.contains(l.inputNodeId)) {
        const EqTarget t = eqTargetByEqId.value(l.inputNodeId, EqTarget{});
        if (t.sinkTargetId != 0) {
          const QString sinkName = nodeNameById.value(t.sinkTargetId);
          const auto eqInIt = portById.find(l.inputPortId);
          if (eqInIt == portById.end()) {
            continue;
          }

          const QString ch = channelKey(*eqInIt);
          const auto sinkPortsIt = inPortsByNodeChannel.find(t.sinkTargetId);
          const PwPortInfo sinkPort = (sinkPortsIt != inPortsByNodeChannel.end())
              ? sinkPortsIt->value(ch, firstInPortByNode.value(t.sinkTargetId, PwPortInfo{}))
              : firstInPortByNode.value(t.sinkTargetId, PwPortInfo{});

          addLink(outNodeName, outPortName, sinkName, sinkPort.name);
          continue;
        }
      }

      // Compress source EQ: eq -> <Y> becomes source -> <Y>
      if (eqNodeIds.contains(l.outputNodeId)) {
        const EqTarget t = eqTargetByEqId.value(l.outputNodeId, EqTarget{});
        if (t.sourceTargetId != 0) {
          const QString sourceName = nodeNameById.value(t.sourceTargetId);
          const auto eqOutIt = portById.find(l.outputPortId);
          if (eqOutIt == portById.end()) {
            continue;
          }

          const QString ch = channelKey(*eqOutIt);
          const auto srcPortsIt = outPortsByNodeChannel.find(t.sourceTargetId);
          const PwPortInfo srcPort = (srcPortsIt != outPortsByNodeChannel.end())
              ? srcPortsIt->value(ch, firstOutPortByNode.value(t.sourceTargetId, PwPortInfo{}))
              : firstOutPortByNode.value(t.sourceTargetId, PwPortInfo{});

          addLink(sourceName, srcPort.name, inNodeName, inPortName);
          continue;
        }
      }

      // Skip any remaining links involving EQ nodes.
      if (eqNodeIds.contains(l.outputNodeId) || eqNodeIds.contains(l.inputNodeId)) {
        continue;
      }

      addLink(outNodeName, outPortName, inNodeName, inPortName);
    }

    std::sort(snap.links.begin(), snap.links.end(), [](const PatchbayLinkSpec& a, const PatchbayLinkSpec& b) {
      if (a.outputNodeName != b.outputNodeName) {
        return a.outputNodeName < b.outputNodeName;
      }
      if (a.outputPortName != b.outputPortName) {
        return a.outputPortName < b.outputPortName;
      }
      if (a.inputNodeName != b.inputNodeName) {
        return a.inputNodeName < b.inputNodeName;
      }
      return a.inputPortName < b.inputPortName;
    });
  }

  // Defaults (store node.name for stability).
  if (const auto sinkId = graph.defaultAudioSinkId()) {
    const auto node = graph.nodeById(*sinkId);
    if (node && !node->name.trimmed().isEmpty()) {
      snap.defaultSinkName = node->name;
    }
  }
  if (const auto sourceId = graph.defaultAudioSourceId()) {
    const auto node = graph.nodeById(*sourceId);
    if (node && !node->name.trimmed().isEmpty()) {
      snap.defaultSourceName = node->name;
    }
  }

  // EQ presets (store effective presets for sinks + sources).
  for (const auto& node : graph.nodes()) {
    if (node.mediaClass != QStringLiteral("Audio/Sink") && node.mediaClass != QStringLiteral("Audio/Source")) {
      continue;
    }
    if (node.name.trimmed().isEmpty()) {
      continue;
    }

    EqPreset preset = defaultEqPreset(6);

    const QString json = s.value(eqPresetKeyForNodeName(node.name)).toString();
    if (!json.trimmed().isEmpty()) {
      QJsonParseError err{};
      const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
      if (err.error == QJsonParseError::NoError && doc.isObject()) {
        preset = eqPresetFromJson(doc.object());
      }
    }

    snap.eqByNodeName.insert(node.name, preset);
  }

  // Layout settings.
  snap.sinksOrder = s.value(SettingsKeys::sinksOrder()).toStringList();

  for (const auto& node : graph.nodes()) {
    if (node.name.trimmed().isEmpty()) {
      continue;
    }
    const QString pos = s.value(SettingsKeys::patchbayLayoutPositionKeyForNodeName(node.name)).toString();
    if (!pos.trimmed().isEmpty()) {
      snap.patchbayPositionByNodeName.insert(node.name, pos);
    }
  }

  return snap;
}
