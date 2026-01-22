#include "SessionSnapshots.h"

#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

namespace {
constexpr const char* kSnapshotDisplayNameKey = "displayName";
constexpr const char* kSnapshotJsonKey = "snapshotJson";

QString snapshotIdForName(const QString& snapshotName)
{
  const QByteArray enc = snapshotName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

std::optional<QString> findSnapshotIdByDisplayName(QSettings& s, const QString& snapshotName)
{
  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
    s.endGroup();
    if (name == snapshotName) {
      s.endGroup();
      return g;
    }
  }
  s.endGroup();
  return std::nullopt;
}

bool isInternalNodeName(const QString& nodeName)
{
  return nodeName.startsWith(QStringLiteral("headroom.meter.")) || nodeName == QStringLiteral("headroom.visualizer") ||
      nodeName == QStringLiteral("headroom.recorder");
}

bool isEqNodeName(const QString& nodeName)
{
  return nodeName.startsWith(QStringLiteral("headroom.eq."));
}

QJsonArray linksToJson(const QVector<PatchbayLinkSpec>& links)
{
  QJsonArray arr;
  for (const auto& l : links) {
    QJsonObject o;
    o.insert(QStringLiteral("outputNode"), l.outputNodeName);
    o.insert(QStringLiteral("outputPort"), l.outputPortName);
    o.insert(QStringLiteral("inputNode"), l.inputNodeName);
    o.insert(QStringLiteral("inputPort"), l.inputPortName);
    arr.append(o);
  }
  return arr;
}

QVector<PatchbayLinkSpec> linksFromJson(const QJsonArray& arr)
{
  QVector<PatchbayLinkSpec> links;
  links.reserve(arr.size());
  for (const auto& v : arr) {
    if (!v.isObject()) {
      continue;
    }
    const QJsonObject o = v.toObject();
    PatchbayLinkSpec l;
    l.outputNodeName = o.value(QStringLiteral("outputNode")).toString();
    l.outputPortName = o.value(QStringLiteral("outputPort")).toString();
    l.inputNodeName = o.value(QStringLiteral("inputNode")).toString();
    l.inputPortName = o.value(QStringLiteral("inputPort")).toString();
    if (l.outputNodeName.isEmpty() || l.outputPortName.isEmpty() || l.inputNodeName.isEmpty() || l.inputPortName.isEmpty()) {
      continue;
    }
    links.push_back(l);
  }
  return links;
}

QJsonObject eqMapToJson(const QHash<QString, EqPreset>& eqByNodeName)
{
  QJsonObject o;
  for (auto it = eqByNodeName.begin(); it != eqByNodeName.end(); ++it) {
    o.insert(it.key(), eqPresetToJson(it.value()));
  }
  return o;
}

QHash<QString, EqPreset> eqMapFromJson(const QJsonObject& o)
{
  QHash<QString, EqPreset> out;
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (!it.value().isObject()) {
      continue;
    }
    out.insert(it.key(), eqPresetFromJson(it.value().toObject()));
  }
  return out;
}

QJsonObject positionsToJson(const QHash<QString, QString>& posByNodeName)
{
  QJsonObject o;
  for (auto it = posByNodeName.begin(); it != posByNodeName.end(); ++it) {
    o.insert(it.key(), it.value());
  }
  return o;
}

QHash<QString, QString> positionsFromJson(const QJsonObject& o)
{
  QHash<QString, QString> out;
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (!it.value().isString()) {
      continue;
    }
    const QString v = it.value().toString();
    if (!v.trimmed().isEmpty()) {
      out.insert(it.key(), v);
    }
  }
  return out;
}

QJsonObject snapshotToJson(const SessionSnapshot& snapshot)
{
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);

  QJsonObject patchbay;
  patchbay.insert(QStringLiteral("links"), linksToJson(snapshot.links));
  root.insert(QStringLiteral("patchbay"), patchbay);

  QJsonObject defaults;
  defaults.insert(QStringLiteral("sink"), snapshot.defaultSinkName);
  defaults.insert(QStringLiteral("source"), snapshot.defaultSourceName);
  root.insert(QStringLiteral("defaults"), defaults);

  root.insert(QStringLiteral("eq"), eqMapToJson(snapshot.eqByNodeName));

  QJsonObject layout;
  QJsonArray order;
  for (const auto& n : snapshot.sinksOrder) {
    order.append(n);
  }
  layout.insert(QStringLiteral("sinksOrder"), order);
  layout.insert(QStringLiteral("patchbayPositions"), positionsToJson(snapshot.patchbayPositionByNodeName));
  root.insert(QStringLiteral("layout"), layout);

  return root;
}

SessionSnapshot snapshotFromJson(const QString& snapshotName, const QJsonObject& root)
{
  SessionSnapshot snapshot;
  snapshot.name = snapshotName.trimmed();

  const QJsonObject patchbay = root.value(QStringLiteral("patchbay")).toObject();
  const QJsonArray linksArr = patchbay.value(QStringLiteral("links")).toArray();
  snapshot.links = linksFromJson(linksArr);

  const QJsonObject defaults = root.value(QStringLiteral("defaults")).toObject();
  snapshot.defaultSinkName = defaults.value(QStringLiteral("sink")).toString();
  snapshot.defaultSourceName = defaults.value(QStringLiteral("source")).toString();

  snapshot.eqByNodeName = eqMapFromJson(root.value(QStringLiteral("eq")).toObject());

  const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
  const QJsonArray sinksOrder = layout.value(QStringLiteral("sinksOrder")).toArray();
  for (const auto& v : sinksOrder) {
    if (v.isString()) {
      snapshot.sinksOrder.push_back(v.toString());
    }
  }
  snapshot.patchbayPositionByNodeName = positionsFromJson(layout.value(QStringLiteral("patchbayPositions")).toObject());
  return snapshot;
}

QString eqPresetKeyForNodeName(const QString& nodeName)
{
  return QStringLiteral("eq/%1/presetJson").arg(nodeName);
}

std::optional<uint32_t> findNodeIdByName(const PipeWireGraph& graph, const QString& nodeName)
{
  if (nodeName.trimmed().isEmpty()) {
    return std::nullopt;
  }
  const auto nodes = graph.nodes();
  for (const auto& n : nodes) {
    if (n.name == nodeName) {
      return n.id;
    }
  }
  return std::nullopt;
}
} // namespace

QStringList SessionSnapshotStore::listSnapshotNames(QSettings& s)
{
  QStringList names;
  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
    s.endGroup();
    if (!name.trimmed().isEmpty()) {
      names.push_back(name);
    }
  }
  s.endGroup();

  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
  return names;
}

std::optional<SessionSnapshot> SessionSnapshotStore::load(QSettings& s, const QString& snapshotName)
{
  const auto idOpt = findSnapshotIdByDisplayName(s, snapshotName);
  if (!idOpt) {
    return std::nullopt;
  }

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.beginGroup(*idOpt);
  const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
  const QString json = s.value(QString::fromUtf8(kSnapshotJsonKey)).toString();
  s.endGroup();
  s.endGroup();

  if (name.trimmed().isEmpty() || json.trimmed().isEmpty()) {
    return std::nullopt;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }

  return snapshotFromJson(name, doc.object());
}

void SessionSnapshotStore::save(QSettings& s, const SessionSnapshot& snapshot)
{
  const QString name = snapshot.name.trimmed();
  if (name.isEmpty()) {
    return;
  }

  const QString id = snapshotIdForName(name);

  const QString json = QString::fromUtf8(QJsonDocument(snapshotToJson(snapshot)).toJson(QJsonDocument::Compact));

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.beginGroup(id);
  s.setValue(QString::fromUtf8(kSnapshotDisplayNameKey), name);
  s.setValue(QString::fromUtf8(kSnapshotJsonKey), json);
  s.endGroup();
  s.endGroup();
}

bool SessionSnapshotStore::remove(QSettings& s, const QString& snapshotName)
{
  const auto idOpt = findSnapshotIdByDisplayName(s, snapshotName);
  if (!idOpt) {
    return false;
  }

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.remove(*idOpt);
  s.endGroup();
  return true;
}

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

SessionSnapshotApplyResult applySessionSnapshot(PipeWireGraph& graph,
                                               QSettings& s,
                                               const SessionSnapshot& snapshot,
                                               bool strictLinks,
                                               bool strictSettings)
{
  SessionSnapshotApplyResult res;

  // Apply patchbay links first.
  PatchbayProfile p;
  p.name = snapshot.name;
  p.links = snapshot.links;
  res.patchbay = applyPatchbayProfile(graph, p, strictLinks);
  res.missing.append(res.patchbay.missing);
  res.errors.append(res.patchbay.errors);

  // Restore defaults (best-effort).
  if (!snapshot.defaultSinkName.trimmed().isEmpty()) {
    res.defaultSinkRequested = true;
    if (!graph.hasDefaultDeviceSupport()) {
      res.errors.push_back(QStringLiteral("PipeWire metadata unavailable; cannot set default output"));
    } else if (const auto id = findNodeIdByName(graph, snapshot.defaultSinkName)) {
      const bool ok = graph.setDefaultAudioSink(*id);
      if (!ok) {
        res.errors.push_back(QStringLiteral("Failed to set default output to %1").arg(snapshot.defaultSinkName));
      } else {
        res.defaultSinkSet = true;
      }
    } else {
      res.missing.push_back(QStringLiteral("Default output node missing: %1").arg(snapshot.defaultSinkName));
    }
  }

  if (!snapshot.defaultSourceName.trimmed().isEmpty()) {
    res.defaultSourceRequested = true;
    if (!graph.hasDefaultDeviceSupport()) {
      res.errors.push_back(QStringLiteral("PipeWire metadata unavailable; cannot set default input"));
    } else if (const auto id = findNodeIdByName(graph, snapshot.defaultSourceName)) {
      const bool ok = graph.setDefaultAudioSource(*id);
      if (!ok) {
        res.errors.push_back(QStringLiteral("Failed to set default input to %1").arg(snapshot.defaultSourceName));
      } else {
        res.defaultSourceSet = true;
      }
    } else {
      res.missing.push_back(QStringLiteral("Default input node missing: %1").arg(snapshot.defaultSourceName));
    }
  }

  // Restore settings (EQ + layout).
  if (strictSettings) {
    s.remove(SettingsKeys::sinksOrder());
    s.remove(SettingsKeys::patchbayLayoutPositionsGroup());
    s.remove(QStringLiteral("eq"));
  }

  if (!snapshot.sinksOrder.isEmpty()) {
    s.setValue(SettingsKeys::sinksOrder(), snapshot.sinksOrder);
  } else if (strictSettings) {
    s.remove(SettingsKeys::sinksOrder());
  }

  for (auto it = snapshot.patchbayPositionByNodeName.begin(); it != snapshot.patchbayPositionByNodeName.end(); ++it) {
    const QString nodeName = it.key();
    const QString pos = it.value();
    if (nodeName.trimmed().isEmpty() || pos.trimmed().isEmpty()) {
      continue;
    }
    s.setValue(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName), pos);
  }

  for (auto it = snapshot.eqByNodeName.begin(); it != snapshot.eqByNodeName.end(); ++it) {
    const QString nodeName = it.key();
    const EqPreset preset = it.value();
    if (nodeName.trimmed().isEmpty()) {
      continue;
    }
    const QString json = QString::fromUtf8(QJsonDocument(eqPresetToJson(preset)).toJson(QJsonDocument::Compact));
    s.setValue(eqPresetKeyForNodeName(nodeName), json);
  }

  return res;
}
