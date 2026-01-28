#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <optional>

#include "backend/PatchbayPortConfig.h"

#include "cli/CliInternal.h"

namespace headroomctl {
int handleGraphConnectionsCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    if (cmd == QStringLiteral("connect")) {
      const bool force = args.contains(QStringLiteral("--force"));
      if (force) {
        args.removeAll(QStringLiteral("--force"));
      }

      if (args.size() < 4) {
        printUsage(err);
        exitCode = 2;
        break;
      }

      const auto outPortId = parseNodeId(args.at(2));
      const auto inPortId = parseNodeId(args.at(3));
      if (!outPortId || !inPortId) {
        err << "headroomctl: connect expects <out-port-id> <in-port-id>\n";
        exitCode = 2;
        break;
      }

      const QList<PwPortInfo> ports = graph.ports();
      const auto outPort = portById(ports, *outPortId);
      const auto inPort = portById(ports, *inPortId);
      if (!outPort || !inPort) {
        err << "headroomctl: unknown port id\n";
        exitCode = 2;
        break;
      }
      if (outPort->direction != QStringLiteral("out") || inPort->direction != QStringLiteral("in")) {
        err << "headroomctl: connect expects an output port then an input port\n";
        exitCode = 2;
        break;
      }

      if (!force) {
        QSettings s;
        const PwNodeInfo outNode = graph.nodeById(outPort->nodeId).value_or(PwNodeInfo{});
        const PwNodeInfo inNode = graph.nodeById(inPort->nodeId).value_or(PwNodeInfo{});
        if (PatchbayPortConfigStore::isLocked(s, outNode.name, outPort->name) || PatchbayPortConfigStore::isLocked(s, inNode.name, inPort->name)) {
          err << "headroomctl: connect blocked (port locked). Use --force to override.\n";
          exitCode = 1;
          break;
        }
      }

      const auto existing = linkByPorts(graph.links(), *outPortId, *inPortId);
      if (existing) {
        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("alreadyExists"), true);
          o.insert(QStringLiteral("linkId"), static_cast<qint64>(existing->id));
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "already-connected\t" << existing->id << "\n";
        }
        exitCode = 0;
        break;
      }

      const bool ok = graph.createLink(outPort->nodeId, outPort->id, inPort->nodeId, inPort->id);
      if (!ok) {
        err << "headroomctl: connect failed\n";
        exitCode = 1;
        break;
      }

      std::optional<PwLinkInfo> created;
      QElapsedTimer t;
      t.start();
      while (t.elapsed() < 750) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        created = linkByPorts(graph.links(), *outPortId, *inPortId);
        if (created) {
          break;
        }
        QThread::msleep(10);
      }

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("alreadyExists"), false);
        if (created) {
          o.insert(QStringLiteral("linkId"), static_cast<qint64>(created->id));
        } else {
          o.insert(QStringLiteral("linkId"), QJsonValue::Null);
        }
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else if (created) {
        out << "connected\t" << created->id << "\n";
      } else {
        out << "connected\t"
            << "unknown-link-id\n";
      }

      exitCode = 0;
      break;
    }

    if (cmd == QStringLiteral("disconnect") || cmd == QStringLiteral("unlink")) {
      const bool force = args.contains(QStringLiteral("--force"));
      if (force) {
        args.removeAll(QStringLiteral("--force"));
      }

      if (args.size() < 3) {
        printUsage(err);
        exitCode = 2;
        break;
      }

      std::optional<uint32_t> linkId;
      if (args.size() == 3) {
        linkId = parseNodeId(args.at(2));
        if (!linkId) {
          err << "headroomctl: disconnect expects <link-id> or <out-port-id> <in-port-id>\n";
          exitCode = 2;
          break;
        }
      } else {
        const auto outPortId = parseNodeId(args.at(2));
        const auto inPortId = parseNodeId(args.at(3));
        if (!outPortId || !inPortId) {
          err << "headroomctl: disconnect expects <link-id> or <out-port-id> <in-port-id>\n";
          exitCode = 2;
          break;
        }
        if (const auto l = linkByPorts(graph.links(), *outPortId, *inPortId)) {
          linkId = l->id;
        }
        if (!linkId) {
          err << "headroomctl: no such link\n";
          exitCode = 1;
          break;
        }
      }

      if (!force) {
        std::optional<PwLinkInfo> l;
        for (const auto& candidate : graph.links()) {
          if (candidate.id == *linkId) {
            l = candidate;
            break;
          }
        }
        if (l) {
          const auto outPort = portById(graph.ports(), l->outputPortId);
          const auto inPort = portById(graph.ports(), l->inputPortId);
          const PwNodeInfo outNode = graph.nodeById(l->outputNodeId).value_or(PwNodeInfo{});
          const PwNodeInfo inNode = graph.nodeById(l->inputNodeId).value_or(PwNodeInfo{});
          QSettings s;
          const QString outPortName = outPort ? outPort->name : QString{};
          const QString inPortName = inPort ? inPort->name : QString{};
          if (PatchbayPortConfigStore::isLocked(s, outNode.name, outPortName) || PatchbayPortConfigStore::isLocked(s, inNode.name, inPortName)) {
            err << "headroomctl: disconnect blocked (port locked). Use --force to override.\n";
            exitCode = 1;
            break;
          }
        }
      }

      const bool ok = graph.destroyLink(*linkId);
      if (!ok) {
        err << "headroomctl: disconnect failed\n";
        exitCode = 1;
        break;
      }

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("linkId"), static_cast<qint64>(*linkId));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "disconnected\t" << *linkId << "\n";
      }

      exitCode = 0;
      break;
    }
  } while (false);

  return exitCode;
}
} // namespace headroomctl

