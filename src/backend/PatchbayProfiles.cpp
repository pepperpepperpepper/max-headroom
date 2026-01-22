#include "PatchbayProfiles.h"

#include "backend/PipeWireGraph.h"
#include "backend/PatchbayPortConfig.h"
#include "settings/SettingsKeys.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSet>

#include <algorithm>

namespace {
constexpr const char* kProfileDisplayNameKey = "displayName";
constexpr const char* kProfileLinksJsonKey = "linksJson";

QString profileIdForName(const QString& profileName)
{
  const QByteArray enc = profileName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

std::optional<QString> findProfileIdByDisplayName(QSettings& s, const QString& profileName)
{
  s.beginGroup(SettingsKeys::patchbayProfilesGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kProfileDisplayNameKey)).toString();
    s.endGroup();
    if (name == profileName) {
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

QString linkText(const PatchbayLinkSpec& l)
{
  return QStringLiteral("%1:%2 -> %3:%4").arg(l.outputNodeName, l.outputPortName, l.inputNodeName, l.inputPortName);
}
} // namespace

QStringList PatchbayProfileStore::listProfileNames(QSettings& s)
{
  QStringList names;
  s.beginGroup(SettingsKeys::patchbayProfilesGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kProfileDisplayNameKey)).toString();
    s.endGroup();
    if (!name.trimmed().isEmpty()) {
      names.push_back(name);
    }
  }
  s.endGroup();

  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
  return names;
}

std::optional<PatchbayProfile> PatchbayProfileStore::load(QSettings& s, const QString& profileName)
{
  const auto idOpt = findProfileIdByDisplayName(s, profileName);
  if (!idOpt) {
    return std::nullopt;
  }

  s.beginGroup(SettingsKeys::patchbayProfilesGroup());
  s.beginGroup(*idOpt);
  PatchbayProfile p;
  p.name = s.value(QString::fromUtf8(kProfileDisplayNameKey)).toString();
  const QString json = s.value(QString::fromUtf8(kProfileLinksJsonKey)).toString();
  s.endGroup();
  s.endGroup();

  if (p.name.trimmed().isEmpty() || json.trimmed().isEmpty()) {
    return std::nullopt;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isArray()) {
    return std::nullopt;
  }

  const QJsonArray arr = doc.array();
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
    p.links.push_back(l);
  }

  return p;
}

void PatchbayProfileStore::save(QSettings& s, const PatchbayProfile& profile)
{
  const QString name = profile.name.trimmed();
  if (name.isEmpty()) {
    return;
  }

  QJsonArray arr;
  for (const auto& l : profile.links) {
    QJsonObject o;
    o.insert(QStringLiteral("outputNode"), l.outputNodeName);
    o.insert(QStringLiteral("outputPort"), l.outputPortName);
    o.insert(QStringLiteral("inputNode"), l.inputNodeName);
    o.insert(QStringLiteral("inputPort"), l.inputPortName);
    arr.append(o);
  }

  const QString id = profileIdForName(name);

  s.beginGroup(SettingsKeys::patchbayProfilesGroup());
  s.beginGroup(id);
  s.setValue(QString::fromUtf8(kProfileDisplayNameKey), name);
  s.setValue(QString::fromUtf8(kProfileLinksJsonKey),
             QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
  s.endGroup();
  s.endGroup();
}

bool PatchbayProfileStore::remove(QSettings& s, const QString& profileName)
{
  const auto idOpt = findProfileIdByDisplayName(s, profileName);
  if (!idOpt) {
    return false;
  }

  s.beginGroup(SettingsKeys::patchbayProfilesGroup());
  s.remove(*idOpt);
  s.endGroup();
  return true;
}

PatchbayProfile snapshotPatchbayProfile(const QString& profileName, const PipeWireGraph& graph)
{
  PatchbayProfile p;
  p.name = profileName.trimmed();

  const QList<PwNodeInfo> nodes = graph.nodes();
  const QList<PwPortInfo> ports = graph.ports();
  const QList<PwLinkInfo> links = graph.links();

  QHash<uint32_t, QString> nodeNameById;
  nodeNameById.reserve(nodes.size());
  QHash<uint32_t, bool> internalNodeById;
  internalNodeById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodeNameById.insert(n.id, n.name);
    internalNodeById.insert(n.id, isInternalNodeName(n.name));
  }

  QHash<uint32_t, PwPortInfo> portById;
  portById.reserve(ports.size());
  for (const auto& port : ports) {
    portById.insert(port.id, port);
  }

  QSet<QString> seen;

  for (const auto& link : links) {
    if (internalNodeById.value(link.outputNodeId, false) || internalNodeById.value(link.inputNodeId, false)) {
      continue;
    }

    const auto outPortIt = portById.find(link.outputPortId);
    const auto inPortIt = portById.find(link.inputPortId);
    if (outPortIt == portById.end() || inPortIt == portById.end()) {
      continue;
    }

    PatchbayLinkSpec l;
    l.outputNodeName = nodeNameById.value(link.outputNodeId);
    l.outputPortName = outPortIt->name;
    l.inputNodeName = nodeNameById.value(link.inputNodeId);
    l.inputPortName = inPortIt->name;
    if (l.outputNodeName.isEmpty() || l.outputPortName.isEmpty() || l.inputNodeName.isEmpty() || l.inputPortName.isEmpty()) {
      continue;
    }

    const QString key = linkText(l);
    if (seen.contains(key)) {
      continue;
    }
    seen.insert(key);
    p.links.push_back(l);
  }

  std::sort(p.links.begin(), p.links.end(), [](const PatchbayLinkSpec& a, const PatchbayLinkSpec& b) {
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

  return p;
}

PatchbayProfileApplyResult applyPatchbayProfile(PipeWireGraph& graph, const PatchbayProfile& profile, bool strict)
{
  PatchbayProfileApplyResult res;
  res.desiredLinks = static_cast<int>(profile.links.size());

  const QList<PwNodeInfo> nodes = graph.nodes();
  const QList<PwPortInfo> ports = graph.ports();
  const QList<PwLinkInfo> links = graph.links();

  QSettings portSettings;

  QHash<uint32_t, QString> nodeNameById;
  nodeNameById.reserve(nodes.size());
  QHash<QString, uint32_t> nodeIdByName;
  nodeIdByName.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodeNameById.insert(n.id, n.name);
    if (!n.name.isEmpty() && !nodeIdByName.contains(n.name)) {
      nodeIdByName.insert(n.name, n.id);
    }
  }

  auto keyFor = [](const QString& nodeName, const QString& portName) { return QStringLiteral("%1\n%2").arg(nodeName, portName); };

  QHash<uint32_t, QString> portNameById;
  portNameById.reserve(ports.size());
  QHash<QString, uint32_t> outPortIdByNodePort;
  QHash<QString, uint32_t> inPortIdByNodePort;
  outPortIdByNodePort.reserve(ports.size());
  inPortIdByNodePort.reserve(ports.size());
  for (const auto& p : ports) {
    const QString nodeName = nodeNameById.value(p.nodeId);
    if (nodeName.isEmpty() || p.name.isEmpty()) {
      continue;
    }
    portNameById.insert(p.id, p.name);
    const QString key = keyFor(nodeName, p.name);
    if (p.direction == QStringLiteral("out")) {
      outPortIdByNodePort.insert(key, p.id);
    } else if (p.direction == QStringLiteral("in")) {
      inPortIdByNodePort.insert(key, p.id);
    }
  }

  auto locked = [&](uint32_t nodeId, uint32_t portId) -> bool {
    const QString nodeName = nodeNameById.value(nodeId);
    const QString portName = portNameById.value(portId);
    return PatchbayPortConfigStore::isLocked(portSettings, nodeName, portName);
  };

  QSet<quint64> existingPairs;
  existingPairs.reserve(links.size());
  for (const auto& l : links) {
    const quint64 pair = (static_cast<quint64>(l.outputPortId) << 32) | static_cast<quint64>(l.inputPortId);
    existingPairs.insert(pair);
  }

  QSet<quint64> desiredPairs;
  desiredPairs.reserve(profile.links.size());
  QSet<uint32_t> involvedPorts;
  involvedPorts.reserve(profile.links.size() * 2);

  struct ResolvedLink final {
    uint32_t outNodeId = 0;
    uint32_t outPortId = 0;
    uint32_t inNodeId = 0;
    uint32_t inPortId = 0;
    quint64 pairKey = 0;
  };
  QVector<ResolvedLink> resolved;
  resolved.reserve(profile.links.size());

  for (const auto& l : profile.links) {
    const uint32_t outNodeId = nodeIdByName.value(l.outputNodeName, 0u);
    const uint32_t inNodeId = nodeIdByName.value(l.inputNodeName, 0u);
    const uint32_t outPortId = outPortIdByNodePort.value(keyFor(l.outputNodeName, l.outputPortName), 0u);
    const uint32_t inPortId = inPortIdByNodePort.value(keyFor(l.inputNodeName, l.inputPortName), 0u);

    if (outNodeId == 0u || inNodeId == 0u || outPortId == 0u || inPortId == 0u) {
      res.missingEndpoints += 1;
      res.missing.push_back(linkText(l));
      continue;
    }

    ResolvedLink r;
    r.outNodeId = outNodeId;
    r.outPortId = outPortId;
    r.inNodeId = inNodeId;
    r.inPortId = inPortId;
    r.pairKey = (static_cast<quint64>(outPortId) << 32) | static_cast<quint64>(inPortId);
    resolved.push_back(r);
    desiredPairs.insert(r.pairKey);
    involvedPorts.insert(outPortId);
    involvedPorts.insert(inPortId);
  }

  if (strict) {
    for (const auto& l : links) {
      const quint64 pair = (static_cast<quint64>(l.outputPortId) << 32) | static_cast<quint64>(l.inputPortId);
      if (desiredPairs.contains(pair)) {
        continue;
      }
      if (!involvedPorts.contains(l.outputPortId) && !involvedPorts.contains(l.inputPortId)) {
        continue;
      }
      if (locked(l.outputNodeId, l.outputPortId) || locked(l.inputNodeId, l.inputPortId)) {
        res.errors.push_back(QStringLiteral("Skipped disconnect (locked port): %1:%2 -> %3:%4")
                                 .arg(nodeNameById.value(l.outputNodeId),
                                      portNameById.value(l.outputPortId, QString::number(l.outputPortId)),
                                      nodeNameById.value(l.inputNodeId),
                                      portNameById.value(l.inputPortId, QString::number(l.inputPortId))));
        continue;
      }
      const bool ok = graph.destroyLink(l.id);
      if (!ok) {
        res.errors.push_back(QStringLiteral("Failed to disconnect link %1").arg(l.id));
        continue;
      }
      existingPairs.remove(pair);
      res.disconnectedLinks += 1;
    }
  }

  for (const auto& r : resolved) {
    if (existingPairs.contains(r.pairKey)) {
      res.alreadyPresentLinks += 1;
      continue;
    }

    if (locked(r.outNodeId, r.outPortId) || locked(r.inNodeId, r.inPortId)) {
      res.errors.push_back(QStringLiteral("Skipped connect (locked port): %1:%2 -> %3:%4")
                               .arg(nodeNameById.value(r.outNodeId),
                                    portNameById.value(r.outPortId, QString::number(r.outPortId)),
                                    nodeNameById.value(r.inNodeId),
                                    portNameById.value(r.inPortId, QString::number(r.inPortId))));
      continue;
    }

    const bool ok = graph.createLink(r.outNodeId, r.outPortId, r.inNodeId, r.inPortId);
    if (!ok) {
      res.errors.push_back(QStringLiteral("Failed to connect %1:%2 -> %3:%4")
                               .arg(nodeNameById.value(r.outNodeId),
                                    portNameById.value(r.outPortId, QString::number(r.outPortId)),
                                    nodeNameById.value(r.inNodeId),
                                    portNameById.value(r.inPortId, QString::number(r.inPortId))));
      continue;
    }
    res.createdLinks += 1;
    existingPairs.insert(r.pairKey);
  }

  return res;
}
