#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class PipeWireGraph;
class QSettings;

struct AutoConnectRule final {
  QString name;
  bool enabled = true;
  QString outputNodeRegex;
  QString outputPortRegex;
  QString inputNodeRegex;
  QString inputPortRegex;
};

struct AutoConnectConfig final {
  bool enabled = false;
  QStringList whitelist; // regexes matching "node.name:port.name"
  QStringList blacklist; // regexes matching "node.name:port.name"
  QVector<AutoConnectRule> rules;
};

struct AutoConnectApplyResult final {
  int rulesConsidered = 0;
  int rulesApplied = 0;
  int linksCreated = 0;
  int linksAlreadyPresent = 0;
  QStringList errors;
};

class AutoConnectRuleStore final
{
public:
  static QStringList listRuleNames(QSettings& s);
  static std::optional<AutoConnectRule> load(QSettings& s, const QString& ruleName);
  static void save(QSettings& s, const AutoConnectRule& rule);
  static bool remove(QSettings& s, const QString& ruleName);
  static QVector<AutoConnectRule> loadAll(QSettings& s);
};

AutoConnectConfig loadAutoConnectConfig(QSettings& s);
void saveAutoConnectConfig(QSettings& s, const AutoConnectConfig& cfg);

AutoConnectApplyResult applyAutoConnectRules(PipeWireGraph& graph, const AutoConnectConfig& cfg);

