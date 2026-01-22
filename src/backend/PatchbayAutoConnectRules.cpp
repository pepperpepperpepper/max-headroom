#include "PatchbayAutoConnectRules.h"

#include "backend/PipeWireGraph.h"
#include "backend/PatchbayPortConfig.h"
#include "settings/SettingsKeys.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>

#include <algorithm>

namespace {
constexpr const char* kRuleDisplayNameKey = "displayName";
constexpr const char* kRuleJsonKey = "ruleJson";

QString ruleIdForName(const QString& ruleName)
{
  const QByteArray enc = ruleName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

std::optional<QString> findRuleIdByDisplayName(QSettings& s, const QString& ruleName)
{
  s.beginGroup(SettingsKeys::patchbayAutoConnectRulesGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kRuleDisplayNameKey)).toString();
    s.endGroup();
    if (name == ruleName) {
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

QString nodeMatchText(const PwNodeInfo& node)
{
  // Allow matching by either internal node.name, or user-visible description/appName/mediaClass.
  return QStringLiteral("%1\n%2\n%3\n%4").arg(node.name, node.description, node.appName, node.mediaClass);
}

QString portMatchText(const PwPortInfo& port, const QString& nodeName, QSettings& s)
{
  const QString custom = PatchbayPortConfigStore::customAlias(s, nodeName, port.name).value_or(QString{});
  // Allow matching by port.name, port.alias, custom alias, and channel label.
  return QStringLiteral("%1\n%2\n%3\n%4").arg(port.name, port.alias, custom, port.audioChannel);
}

QVector<QRegularExpression> compileEndpointFilters(const QStringList& patterns, QStringList* errors, const QString& label)
{
  QVector<QRegularExpression> out;
  out.reserve(patterns.size());
  for (const auto& raw : patterns) {
    const QString p = raw.trimmed();
    if (p.isEmpty()) {
      continue;
    }
    QRegularExpression re(p);
    if (!re.isValid()) {
      if (errors) {
        errors->push_back(QStringLiteral("Invalid %1 regex: %2").arg(label, p));
      }
      continue;
    }
    out.push_back(re);
  }
  return out;
}

QString endpointText(const QString& nodeName, const QString& portName)
{
  return QStringLiteral("%1:%2").arg(nodeName, portName);
}
} // namespace

QStringList AutoConnectRuleStore::listRuleNames(QSettings& s)
{
  QStringList names;
  s.beginGroup(SettingsKeys::patchbayAutoConnectRulesGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kRuleDisplayNameKey)).toString();
    s.endGroup();
    if (!name.trimmed().isEmpty()) {
      names.push_back(name);
    }
  }
  s.endGroup();

  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
  return names;
}

std::optional<AutoConnectRule> AutoConnectRuleStore::load(QSettings& s, const QString& ruleName)
{
  const auto idOpt = findRuleIdByDisplayName(s, ruleName);
  if (!idOpt) {
    return std::nullopt;
  }

  s.beginGroup(SettingsKeys::patchbayAutoConnectRulesGroup());
  s.beginGroup(*idOpt);
  AutoConnectRule r;
  r.name = s.value(QString::fromUtf8(kRuleDisplayNameKey)).toString();
  const QString json = s.value(QString::fromUtf8(kRuleJsonKey)).toString();
  s.endGroup();
  s.endGroup();

  if (r.name.trimmed().isEmpty() || json.trimmed().isEmpty()) {
    return std::nullopt;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }

  const QJsonObject o = doc.object();
  r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
  r.outputNodeRegex = o.value(QStringLiteral("outputNodeRegex")).toString();
  r.outputPortRegex = o.value(QStringLiteral("outputPortRegex")).toString();
  r.inputNodeRegex = o.value(QStringLiteral("inputNodeRegex")).toString();
  r.inputPortRegex = o.value(QStringLiteral("inputPortRegex")).toString();

  if (r.outputNodeRegex.trimmed().isEmpty() || r.outputPortRegex.trimmed().isEmpty() || r.inputNodeRegex.trimmed().isEmpty() ||
      r.inputPortRegex.trimmed().isEmpty()) {
    return std::nullopt;
  }

  return r;
}

void AutoConnectRuleStore::save(QSettings& s, const AutoConnectRule& rule)
{
  const QString name = rule.name.trimmed();
  if (name.isEmpty()) {
    return;
  }

  QJsonObject o;
  o.insert(QStringLiteral("enabled"), rule.enabled);
  o.insert(QStringLiteral("outputNodeRegex"), rule.outputNodeRegex);
  o.insert(QStringLiteral("outputPortRegex"), rule.outputPortRegex);
  o.insert(QStringLiteral("inputNodeRegex"), rule.inputNodeRegex);
  o.insert(QStringLiteral("inputPortRegex"), rule.inputPortRegex);

  const QString id = ruleIdForName(name);
  s.beginGroup(SettingsKeys::patchbayAutoConnectRulesGroup());
  s.beginGroup(id);
  s.setValue(QString::fromUtf8(kRuleDisplayNameKey), name);
  s.setValue(QString::fromUtf8(kRuleJsonKey),
             QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
  s.endGroup();
  s.endGroup();
}

bool AutoConnectRuleStore::remove(QSettings& s, const QString& ruleName)
{
  const auto idOpt = findRuleIdByDisplayName(s, ruleName);
  if (!idOpt) {
    return false;
  }

  s.beginGroup(SettingsKeys::patchbayAutoConnectRulesGroup());
  s.remove(*idOpt);
  s.endGroup();
  return true;
}

QVector<AutoConnectRule> AutoConnectRuleStore::loadAll(QSettings& s)
{
  QVector<AutoConnectRule> rules;
  const QStringList names = listRuleNames(s);
  rules.reserve(names.size());
  for (const auto& n : names) {
    const auto rule = load(s, n);
    if (rule) {
      rules.push_back(*rule);
    }
  }
  return rules;
}

AutoConnectConfig loadAutoConnectConfig(QSettings& s)
{
  AutoConnectConfig cfg;
  cfg.enabled = s.value(SettingsKeys::patchbayAutoConnectEnabled()).toBool();
  cfg.whitelist = s.value(SettingsKeys::patchbayAutoConnectWhitelist()).toStringList();
  cfg.blacklist = s.value(SettingsKeys::patchbayAutoConnectBlacklist()).toStringList();
  cfg.rules = AutoConnectRuleStore::loadAll(s);
  return cfg;
}

void saveAutoConnectConfig(QSettings& s, const AutoConnectConfig& cfg)
{
  if (cfg.enabled) {
    s.setValue(SettingsKeys::patchbayAutoConnectEnabled(), true);
  } else {
    s.remove(SettingsKeys::patchbayAutoConnectEnabled());
  }

  auto normalize = [](const QStringList& in) {
    QStringList out;
    out.reserve(in.size());
    for (const auto& v : in) {
      const QString t = v.trimmed();
      if (!t.isEmpty()) {
        out.push_back(t);
      }
    }
    out.removeDuplicates();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
    return out;
  };

  const QStringList wl = normalize(cfg.whitelist);
  const QStringList bl = normalize(cfg.blacklist);
  if (!wl.isEmpty()) {
    s.setValue(SettingsKeys::patchbayAutoConnectWhitelist(), wl);
  } else {
    s.remove(SettingsKeys::patchbayAutoConnectWhitelist());
  }
  if (!bl.isEmpty()) {
    s.setValue(SettingsKeys::patchbayAutoConnectBlacklist(), bl);
  } else {
    s.remove(SettingsKeys::patchbayAutoConnectBlacklist());
  }

  const QStringList prev = AutoConnectRuleStore::listRuleNames(s);
  QSet<QString> keep;
  keep.reserve(cfg.rules.size());
  for (const auto& rule : cfg.rules) {
    if (rule.name.trimmed().isEmpty()) {
      continue;
    }
    AutoConnectRuleStore::save(s, rule);
    keep.insert(rule.name.trimmed());
  }
  for (const auto& name : prev) {
    if (!keep.contains(name)) {
      AutoConnectRuleStore::remove(s, name);
    }
  }
}

AutoConnectApplyResult applyAutoConnectRules(PipeWireGraph& graph, const AutoConnectConfig& cfg)
{
  AutoConnectApplyResult res;

  const QList<PwNodeInfo> nodes = graph.nodes();
  const QList<PwPortInfo> ports = graph.ports();
  const QList<PwLinkInfo> links = graph.links();

  QSettings portSettings;

  QHash<uint32_t, PwNodeInfo> nodeById;
  nodeById.reserve(nodes.size());
  QHash<uint32_t, QString> nodeNameById;
  nodeNameById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodeById.insert(n.id, n);
    nodeNameById.insert(n.id, n.name);
  }

  QSet<quint64> existingPairs;
  existingPairs.reserve(links.size());
  for (const auto& l : links) {
    existingPairs.insert((static_cast<quint64>(l.outputPortId) << 32) | static_cast<quint64>(l.inputPortId));
  }

  QStringList filterErrors;
  const QVector<QRegularExpression> whitelist = compileEndpointFilters(cfg.whitelist, &filterErrors, QStringLiteral("whitelist"));
  const QVector<QRegularExpression> blacklist = compileEndpointFilters(cfg.blacklist, &filterErrors, QStringLiteral("blacklist"));
  res.errors.append(filterErrors);

  auto endpointAllowed = [&](const QString& nodeName, const QString& portName) -> bool {
    if (nodeName.isEmpty() || portName.isEmpty()) {
      return false;
    }
    if (isInternalNodeName(nodeName)) {
      return false;
    }
    if (PatchbayPortConfigStore::isLocked(portSettings, nodeName, portName)) {
      return false;
    }
    const QString text = endpointText(nodeName, portName);

    if (!whitelist.isEmpty()) {
      bool ok = false;
      for (const auto& re : whitelist) {
        if (re.match(text).hasMatch()) {
          ok = true;
          break;
        }
      }
      if (!ok) {
        return false;
      }
    }

    for (const auto& re : blacklist) {
      if (re.match(text).hasMatch()) {
        return false;
      }
    }

    return true;
  };

  auto portLess = [&](const PwPortInfo& a, const PwPortInfo& b) {
    const QString an = nodeNameById.value(a.nodeId);
    const QString bn = nodeNameById.value(b.nodeId);
    if (an != bn) {
      return an < bn;
    }
    return a.name < b.name;
  };

  auto channelEqual = [](const QString& a, const QString& b) {
    return !a.isEmpty() && !b.isEmpty() && a.compare(b, Qt::CaseInsensitive) == 0;
  };

  for (const auto& rule : cfg.rules) {
    res.rulesConsidered += 1;
    if (!rule.enabled) {
      continue;
    }

    QRegularExpression outNodeRe(rule.outputNodeRegex);
    QRegularExpression outPortRe(rule.outputPortRegex);
    QRegularExpression inNodeRe(rule.inputNodeRegex);
    QRegularExpression inPortRe(rule.inputPortRegex);

    if (!outNodeRe.isValid() || !outPortRe.isValid() || !inNodeRe.isValid() || !inPortRe.isValid()) {
      res.errors.push_back(QStringLiteral("Rule “%1” has an invalid regex.").arg(rule.name));
      continue;
    }

    res.rulesApplied += 1;

    QVector<PwPortInfo> matchedOut;
    QVector<PwPortInfo> matchedIn;
    matchedOut.reserve(64);
    matchedIn.reserve(64);

    for (const auto& p : ports) {
      if (p.name.trimmed().isEmpty()) {
        continue;
      }
      if (!nodeById.contains(p.nodeId)) {
        continue;
      }
      const PwNodeInfo node = nodeById.value(p.nodeId);
      if (!endpointAllowed(node.name, p.name)) {
        continue;
      }

      const QString nText = nodeMatchText(node);
      const QString pText = portMatchText(p, node.name, portSettings);

      if (p.direction == QStringLiteral("out")) {
        if (outNodeRe.match(nText).hasMatch() && outPortRe.match(pText).hasMatch()) {
          matchedOut.push_back(p);
        }
      } else if (p.direction == QStringLiteral("in")) {
        if (inNodeRe.match(nText).hasMatch() && inPortRe.match(pText).hasMatch()) {
          matchedIn.push_back(p);
        }
      }
    }

    if (matchedOut.isEmpty() || matchedIn.isEmpty()) {
      continue;
    }

    std::sort(matchedIn.begin(), matchedIn.end(), portLess);

    QHash<uint32_t, QVector<PwPortInfo>> outsByNode;
    outsByNode.reserve(matchedOut.size());
    for (const auto& p : matchedOut) {
      outsByNode[p.nodeId].push_back(p);
    }

    for (auto it = outsByNode.begin(); it != outsByNode.end(); ++it) {
      auto& outs = it.value();
      std::sort(outs.begin(), outs.end(), [](const PwPortInfo& a, const PwPortInfo& b) { return a.name < b.name; });

      for (int i = 0; i < outs.size(); ++i) {
        const PwPortInfo& outp = outs[i];

        const PwPortInfo* inChoice = nullptr;
        if (!outp.audioChannel.isEmpty()) {
          for (const auto& inp : matchedIn) {
            if (channelEqual(outp.audioChannel, inp.audioChannel)) {
              inChoice = &inp;
              break;
            }
          }
        }
        if (!inChoice) {
          for (const auto& inp : matchedIn) {
            if (inp.name == outp.name) {
              inChoice = &inp;
              break;
            }
          }
        }
        if (!inChoice) {
          const int idx = std::clamp(i, 0, static_cast<int>(matchedIn.size()) - 1);
          inChoice = &matchedIn[idx];
        }

        if (!inChoice) {
          continue;
        }

        const quint64 pair = (static_cast<quint64>(outp.id) << 32) | static_cast<quint64>(inChoice->id);
        if (existingPairs.contains(pair)) {
          res.linksAlreadyPresent += 1;
          continue;
        }

        const bool ok = graph.createLink(outp.nodeId, outp.id, inChoice->nodeId, inChoice->id);
        if (!ok) {
          res.errors.push_back(QStringLiteral("Failed to connect (rule “%1”): %2 -> %3")
                                   .arg(rule.name,
                                        endpointText(nodeNameById.value(outp.nodeId), outp.name),
                                        endpointText(nodeNameById.value(inChoice->nodeId), inChoice->name)));
          continue;
        }
        existingPairs.insert(pair);
        res.linksCreated += 1;
      }
    }
  }

  return res;
}
