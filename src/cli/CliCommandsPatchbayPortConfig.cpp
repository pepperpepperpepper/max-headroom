#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTextStream>

#include "backend/PatchbayPortConfig.h"
#include "cli/CliInternal.h"

namespace headroomctl {
int handlePatchbayPortConfigSubcommand(QStringList args, PipeWireGraph& graph, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    if (args.size() < 5) {
      printUsage(err);
      exitCode = 2;
      break;
    }

    const QString sub2 = args.at(3).trimmed().toLower();

    auto loadEndpoint = [&](uint32_t portId, PwPortInfo* outPort, PwNodeInfo* outNode) -> bool {
      const auto portOpt = portById(graph.ports(), portId);
      if (!portOpt) {
        return false;
      }
      const PwPortInfo port = *portOpt;
      const PwNodeInfo node = graph.nodeById(port.nodeId).value_or(PwNodeInfo{});
      if (outPort) {
        *outPort = port;
      }
      if (outNode) {
        *outNode = node;
      }
      return true;
    };

    if (sub2 == QStringLiteral("status")) {
      const auto portId = parseNodeId(args.at(4));
      if (!portId) {
        err << "headroomctl: patchbay port status expects <port-id>\n";
        exitCode = 2;
        break;
      }

      PwPortInfo port;
      PwNodeInfo node;
      if (!loadEndpoint(*portId, &port, &node)) {
        err << "headroomctl: unknown port id\n";
        exitCode = 2;
        break;
      }

      const PatchbayPortConfig cfg = PatchbayPortConfigStore::load(s, node.name, port.name);

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("portId"), static_cast<qint64>(port.id));
        o.insert(QStringLiteral("nodeId"), static_cast<qint64>(port.nodeId));
        o.insert(QStringLiteral("nodeName"), node.name);
        o.insert(QStringLiteral("portName"), port.name);
        o.insert(QStringLiteral("customAlias"), cfg.customAlias.value_or(QString{}));
        o.insert(QStringLiteral("locked"), cfg.locked);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "port\t" << port.id << "\n";
        out << "node\t" << node.name << "\n";
        out << "name\t" << port.name << "\n";
        out << "custom-alias\t" << cfg.customAlias.value_or(QStringLiteral("")) << "\n";
        out << "locked\t" << (cfg.locked ? "yes" : "no") << "\n";
      }
      exitCode = 0;
      break;
    }

    if (sub2 == QStringLiteral("alias")) {
      if (args.size() < 6) {
        err << "headroomctl: patchbay port alias expects get|set|clear <port-id> ...\n";
        exitCode = 2;
        break;
      }
      const QString op = args.at(4).trimmed().toLower();
      const auto portId = parseNodeId(args.at(5));
      if (!portId) {
        err << "headroomctl: patchbay port alias expects <port-id>\n";
        exitCode = 2;
        break;
      }

      PwPortInfo port;
      PwNodeInfo node;
      if (!loadEndpoint(*portId, &port, &node)) {
        err << "headroomctl: unknown port id\n";
        exitCode = 2;
        break;
      }

      if (op == QStringLiteral("get")) {
        const auto a = PatchbayPortConfigStore::customAlias(s, node.name, port.name);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("portId"), static_cast<qint64>(port.id));
          o.insert(QStringLiteral("nodeName"), node.name);
          o.insert(QStringLiteral("portName"), port.name);
          if (a) {
            o.insert(QStringLiteral("customAlias"), *a);
          } else {
            o.insert(QStringLiteral("customAlias"), QJsonValue::Null);
          }
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else if (a) {
          out << *a << "\n";
        }
        exitCode = 0;
        break;
      }

      if (op == QStringLiteral("set")) {
        if (args.size() < 7) {
          err << "headroomctl: patchbay port alias set expects <port-id> <alias>\n";
          exitCode = 2;
          break;
        }
        const QString alias = args.mid(6).join(' ').trimmed();
        PatchbayPortConfigStore::setCustomAlias(s, node.name, port.name, alias);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("customAlias"), alias);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "alias\t" << alias << "\n";
        }
        exitCode = 0;
        break;
      }

      if (op == QStringLiteral("clear")) {
        PatchbayPortConfigStore::clearCustomAlias(s, node.name, port.name);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "cleared\n";
        }
        exitCode = 0;
        break;
      }

      err << "headroomctl: patchbay port alias expects get|set|clear\n";
      exitCode = 2;
      break;
    }

    if (sub2 == QStringLiteral("lock")) {
      if (args.size() < 6) {
        err << "headroomctl: patchbay port lock expects get|set <port-id> ...\n";
        exitCode = 2;
        break;
      }
      const QString op = args.at(4).trimmed().toLower();
      const auto portId = parseNodeId(args.at(5));
      if (!portId) {
        err << "headroomctl: patchbay port lock expects <port-id>\n";
        exitCode = 2;
        break;
      }

      PwPortInfo port;
      PwNodeInfo node;
      if (!loadEndpoint(*portId, &port, &node)) {
        err << "headroomctl: unknown port id\n";
        exitCode = 2;
        break;
      }

      if (op == QStringLiteral("get")) {
        const bool locked = PatchbayPortConfigStore::isLocked(s, node.name, port.name);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("locked"), locked);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << (locked ? "locked" : "unlocked") << "\n";
        }
        exitCode = 0;
        break;
      }

      if (op == QStringLiteral("set")) {
        if (args.size() < 7) {
          err << "headroomctl: patchbay port lock set expects <port-id> on|off|toggle\n";
          exitCode = 2;
          break;
        }
        const QString mode = args.at(6).trimmed().toLower();
        bool target = PatchbayPortConfigStore::isLocked(s, node.name, port.name);
        if (mode == QStringLiteral("on")) {
          target = true;
        } else if (mode == QStringLiteral("off")) {
          target = false;
        } else if (mode == QStringLiteral("toggle")) {
          target = !target;
        } else {
          err << "headroomctl: patchbay port lock set expects on|off|toggle\n";
          exitCode = 2;
          break;
        }
        PatchbayPortConfigStore::setLocked(s, node.name, port.name, target);
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("locked"), target);
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "locked\t" << (target ? "yes" : "no") << "\n";
        }
        exitCode = 0;
        break;
      }

      err << "headroomctl: patchbay port lock expects get|set\n";
      exitCode = 2;
      break;
    }

    err << "headroomctl: patchbay port expects status|alias|lock\n";
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl

