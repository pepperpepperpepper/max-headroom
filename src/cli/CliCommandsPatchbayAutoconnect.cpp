#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

#include "backend/PatchbayAutoConnectRules.h"
#include "cli/CliInternal.h"

namespace headroomctl {
int handlePatchbayAutoconnectSubcommand(QStringList args, PipeWireGraph& graph, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    if (args.size() < 4) {
      printUsage(err);
      exitCode = 2;
      break;
    }

    QStringList rest = args.mid(3);
    const QString sub2 = rest.value(0).trimmed().toLower();

    auto loadCfg = [&]() {
      AutoConnectConfig cfg = loadAutoConnectConfig(s);
      return cfg;
    };

    if (sub2 == QStringLiteral("status")) {
      const AutoConnectConfig cfg = loadCfg();
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("enabled"), cfg.enabled);
        o.insert(QStringLiteral("rules"), static_cast<int>(cfg.rules.size()));
        o.insert(QStringLiteral("whitelist"), static_cast<int>(cfg.whitelist.size()));
        o.insert(QStringLiteral("blacklist"), static_cast<int>(cfg.blacklist.size()));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "enabled\t" << (cfg.enabled ? "on" : "off") << "\n";
        out << "rules\t" << cfg.rules.size() << "\n";
        out << "whitelist\t" << cfg.whitelist.size() << "\n";
        out << "blacklist\t" << cfg.blacklist.size() << "\n";
      }
      exitCode = 0;
      break;
    }

    if (sub2 == QStringLiteral("enable")) {
      if (rest.size() < 2) {
        err << "headroomctl: patchbay autoconnect enable expects on|off|toggle\n";
        exitCode = 2;
        break;
      }
      AutoConnectConfig cfg = loadCfg();
      const QString mode = rest.value(1).trimmed().toLower();
      if (mode == QStringLiteral("on")) {
        cfg.enabled = true;
      } else if (mode == QStringLiteral("off")) {
        cfg.enabled = false;
      } else if (mode == QStringLiteral("toggle")) {
        cfg.enabled = !cfg.enabled;
      } else {
        err << "headroomctl: patchbay autoconnect enable expects on|off|toggle\n";
        exitCode = 2;
        break;
      }
      saveAutoConnectConfig(s, cfg);
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("enabled"), cfg.enabled);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "enabled\t" << (cfg.enabled ? "on" : "off") << "\n";
      }
      exitCode = 0;
      break;
    }

    if (sub2 == QStringLiteral("apply")) {
      const AutoConnectConfig cfg = loadCfg();
      const AutoConnectApplyResult r = applyAutoConnectRules(graph, cfg);
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), r.errors.isEmpty());
        o.insert(QStringLiteral("createdLinks"), r.linksCreated);
        o.insert(QStringLiteral("alreadyPresentLinks"), r.linksAlreadyPresent);
        o.insert(QStringLiteral("rulesConsidered"), r.rulesConsidered);
        o.insert(QStringLiteral("rulesApplied"), r.rulesApplied);
        QJsonArray errors;
        for (const auto& e : r.errors) {
          errors.append(e);
        }
        o.insert(QStringLiteral("errors"), errors);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "applied\tcreated=" << r.linksCreated << "\talready=" << r.linksAlreadyPresent << "\trules=" << r.rulesApplied << "\terrors="
            << r.errors.size() << "\n";
        for (const auto& e : r.errors) {
          err << "error:\t" << e << "\n";
        }
      }
      exitCode = r.errors.isEmpty() ? 0 : 1;
      break;
    }

    if (sub2 == QStringLiteral("rules")) {
      const AutoConnectConfig cfg = loadCfg();
      QVector<AutoConnectRule> rules = cfg.rules;
      std::sort(rules.begin(), rules.end(), [](const AutoConnectRule& a, const AutoConnectRule& b) { return a.name.toLower() < b.name.toLower(); });
      if (jsonOutput) {
        QJsonArray arr;
        for (const auto& r : rules) {
          QJsonObject o;
          o.insert(QStringLiteral("name"), r.name);
          o.insert(QStringLiteral("enabled"), r.enabled);
          o.insert(QStringLiteral("outputNodeRegex"), r.outputNodeRegex);
          o.insert(QStringLiteral("outputPortRegex"), r.outputPortRegex);
          o.insert(QStringLiteral("inputNodeRegex"), r.inputNodeRegex);
          o.insert(QStringLiteral("inputPortRegex"), r.inputPortRegex);
          arr.append(o);
        }
        out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        for (const auto& r : rules) {
          out << r.name << "\t" << (r.enabled ? "enabled" : "disabled") << "\t" << r.outputNodeRegex << "/" << r.outputPortRegex << " -> "
              << r.inputNodeRegex << "/" << r.inputPortRegex << "\n";
        }
      }
      exitCode = 0;
      break;
    }

    if (sub2 == QStringLiteral("rule")) {
      if (rest.size() < 2) {
        printUsage(err);
        exitCode = 2;
        break;
      }
      const QString sub3 = rest.value(1).trimmed().toLower();

      if (sub3 == QStringLiteral("add")) {
        const bool disabled = rest.contains(QStringLiteral("--disabled"));
        if (disabled) {
          rest.removeAll(QStringLiteral("--disabled"));
        }
        if (rest.size() < 7) {
          err << "headroomctl: patchbay autoconnect rule add expects <name> <out-node-re> <out-port-re> <in-node-re> <in-port-re>\n";
          exitCode = 2;
          break;
        }
        AutoConnectRule r;
        r.name = rest.value(2).trimmed();
        r.outputNodeRegex = rest.value(3).trimmed();
        r.outputPortRegex = rest.value(4).trimmed();
        r.inputNodeRegex = rest.value(5).trimmed();
        r.inputPortRegex = rest.value(6).trimmed();
        r.enabled = !disabled;
        if (r.name.isEmpty() || r.outputNodeRegex.isEmpty() || r.outputPortRegex.isEmpty() || r.inputNodeRegex.isEmpty() || r.inputPortRegex.isEmpty()) {
          err << "headroomctl: patchbay autoconnect rule add expects non-empty fields\n";
          exitCode = 2;
          break;
        }
        AutoConnectRuleStore::save(s, r);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("name"), r.name);
          o.insert(QStringLiteral("enabled"), r.enabled);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "saved\t" << r.name << "\t" << (r.enabled ? "enabled" : "disabled") << "\n";
        }
        exitCode = 0;
        break;
      }

      if (sub3 == QStringLiteral("enable")) {
        if (rest.size() < 4) {
          err << "headroomctl: patchbay autoconnect rule enable expects <name> on|off|toggle\n";
          exitCode = 2;
          break;
        }
        const QString name = rest.value(2).trimmed();
        const QString mode = rest.value(3).trimmed().toLower();
        if (name.isEmpty()) {
          err << "headroomctl: patchbay autoconnect rule enable expects <name>\n";
          exitCode = 2;
          break;
        }
        auto rule = AutoConnectRuleStore::load(s, name);
        if (!rule) {
          err << "headroomctl: autoconnect rule not found: " << name << "\n";
          exitCode = 1;
          break;
        }
        if (mode == QStringLiteral("on")) {
          rule->enabled = true;
        } else if (mode == QStringLiteral("off")) {
          rule->enabled = false;
        } else if (mode == QStringLiteral("toggle")) {
          rule->enabled = !rule->enabled;
        } else {
          err << "headroomctl: patchbay autoconnect rule enable expects on|off|toggle\n";
          exitCode = 2;
          break;
        }
        AutoConnectRuleStore::save(s, *rule);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("name"), rule->name);
          o.insert(QStringLiteral("enabled"), rule->enabled);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "enabled\t" << rule->name << "\t" << (rule->enabled ? "on" : "off") << "\n";
        }
        exitCode = 0;
        break;
      }

      if (sub3 == QStringLiteral("delete")) {
        if (rest.size() < 3) {
          err << "headroomctl: patchbay autoconnect rule delete expects <name>\n";
          exitCode = 2;
          break;
        }
        const QString name = rest.value(2).trimmed();
        if (name.isEmpty()) {
          err << "headroomctl: patchbay autoconnect rule delete expects <name>\n";
          exitCode = 2;
          break;
        }
        const bool removed = AutoConnectRuleStore::remove(s, name);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), removed);
          o.insert(QStringLiteral("name"), name);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << (removed ? "deleted" : "not-found") << "\t" << name << "\n";
        }
        exitCode = removed ? 0 : 1;
        break;
      }

      printUsage(err);
      exitCode = 2;
      break;
    }

    auto handleListMutate = [&](const QString& which, bool isWhitelist) {
      AutoConnectConfig cfg = loadCfg();
      QStringList& list = isWhitelist ? cfg.whitelist : cfg.blacklist;

      if (rest.size() < 2) {
        printUsage(err);
        exitCode = 2;
        return;
      }
      const QString op = rest.value(1).trimmed().toLower();
      if (op == QStringLiteral("list")) {
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& v : list) {
            arr.append(v);
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          for (const auto& v : list) {
            out << v << "\n";
          }
        }
        exitCode = 0;
        return;
      }

      if (op == QStringLiteral("add")) {
        if (rest.size() < 3) {
          err << "headroomctl: patchbay autoconnect " << which << " add expects <regex>\n";
          exitCode = 2;
          return;
        }
        const QString value = rest.value(2).trimmed();
        if (!value.isEmpty()) {
          list.push_back(value);
        }
        saveAutoConnectConfig(s, cfg);
        exitCode = 0;
        return;
      }

      if (op == QStringLiteral("remove")) {
        if (rest.size() < 3) {
          err << "headroomctl: patchbay autoconnect " << which << " remove expects <regex>\n";
          exitCode = 2;
          return;
        }
        const QString value = rest.value(2).trimmed();
        list.removeAll(value);
        saveAutoConnectConfig(s, cfg);
        exitCode = 0;
        return;
      }

      printUsage(err);
      exitCode = 2;
    };

    if (sub2 == QStringLiteral("whitelist")) {
      handleListMutate(QStringLiteral("whitelist"), true);
      break;
    }
    if (sub2 == QStringLiteral("blacklist")) {
      handleListMutate(QStringLiteral("blacklist"), false);
      break;
    }

    printUsage(err);
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl

