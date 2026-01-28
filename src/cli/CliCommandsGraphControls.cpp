#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTextStream>

#include <optional>

#include "cli/CliInternal.h"

namespace headroomctl {
int handleGraphControlsCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    if (cmd == QStringLiteral("default-sink") || cmd == QStringLiteral("default-source")) {
      const bool wantSink = (cmd == QStringLiteral("default-sink"));
      const QString sub = (args.size() >= 3) ? args.at(2).trimmed().toLower() : QStringLiteral("get");

      auto resolveDefaultNodeId = [&](const QString& idOrName) -> std::optional<uint32_t> {
        const QString key = idOrName.trimmed();
        bool ok = false;
        const uint32_t id = key.toUInt(&ok);
        if (ok) {
          return id;
        }

        const QList<PwNodeInfo> candidates = wantSink ? graph.audioSinks() : graph.audioSources();
        for (const auto& n : candidates) {
          if (n.name == key || n.description == key || nodeLabel(n) == key) {
            return n.id;
          }
        }
        return std::nullopt;
      };

      auto printCurrent = [&]() {
        const std::optional<uint32_t> idOpt = wantSink ? graph.defaultAudioSinkId() : graph.defaultAudioSourceId();
        const PwNodeInfo n = idOpt.has_value() ? graph.nodeById(*idOpt).value_or(PwNodeInfo{}) : PwNodeInfo{};

        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          if (wantSink) {
            o.insert(QStringLiteral("defaultSinkId"), idOpt.has_value() ? QJsonValue(static_cast<qint64>(*idOpt)) : QJsonValue());
            if (n.id != 0u) {
              o.insert(QStringLiteral("defaultSink"), nodeToJson(n));
            } else {
              o.insert(QStringLiteral("defaultSink"), QJsonValue());
            }
          } else {
            o.insert(QStringLiteral("defaultSourceId"), idOpt.has_value() ? QJsonValue(static_cast<qint64>(*idOpt)) : QJsonValue());
            if (n.id != 0u) {
              o.insert(QStringLiteral("defaultSource"), nodeToJson(n));
            } else {
              o.insert(QStringLiteral("defaultSource"), QJsonValue());
            }
          }
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          if (!idOpt.has_value()) {
            out << "none\n";
          } else {
            out << *idOpt << "\t" << nodeLabel(n) << "\n";
          }
        }
      };

      if (sub == QStringLiteral("get")) {
        printCurrent();
        exitCode = 0;
        break;
      }

      if (sub == QStringLiteral("set")) {
        if (args.size() < 4) {
          err << "headroomctl: " << cmd << " set expects <node-id|node-name>\n";
          exitCode = 2;
          break;
        }
        if (!graph.hasDefaultDeviceSupport()) {
          err << "headroomctl: default device controls unavailable (settings metadata missing)\n";
          exitCode = 1;
          break;
        }

        const auto idOpt = resolveDefaultNodeId(args.at(3));
        if (!idOpt.has_value()) {
          err << "headroomctl: unknown node (expected a sink/source id or name)\n";
          exitCode = 2;
          break;
        }

        bool ok = false;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
          ok = wantSink ? graph.setDefaultAudioSink(*idOpt) : graph.setDefaultAudioSource(*idOpt);
          if (ok) {
            break;
          }
          waitForGraph(60);
        }
        if (!ok) {
          err << "headroomctl: failed to set default device\n";
          exitCode = 1;
          break;
        }

        waitForGraph(120);
        printCurrent();
        exitCode = 0;
        break;
      }

      err << "headroomctl: " << cmd << " expects [get] or set <node-id|node-name>\n";
      exitCode = 2;
      break;
    }

    if (cmd == QStringLiteral("set-volume")) {
      if (args.size() < 4) {
        printUsage(err);
        exitCode = 2;
        break;
      }

      const auto id = parseNodeId(args.at(2));
      const auto volume = parseVolumeValue(args.at(3));
      if (!id || !volume) {
        err << "headroomctl: invalid node-id or volume value\n";
        exitCode = 2;
        break;
      }

      // Give the PipeWire registry a moment to populate; for one-shot commands,
      // graph discovery can race the initial setNodeVolume call.
      bool ok = false;
      QElapsedTimer t;
      t.start();
      while (t.elapsed() < 1500) {
        ok = graph.setNodeVolume(*id, *volume);
        if (ok) {
          break;
        }
        waitForGraph(60);
      }
      if (!ok) {
        err << "headroomctl: failed to set volume\n";
        exitCode = 1;
        break;
      }

      // Give PipeWire a short moment to flush the request before exiting.
      waitForGraph(120);

      exitCode = 0;
      break;
    }

    if (cmd == QStringLiteral("mute")) {
      if (args.size() < 4) {
        printUsage(err);
        exitCode = 2;
        break;
      }

      const auto id = parseNodeId(args.at(2));
      if (!id) {
        err << "headroomctl: invalid node-id\n";
        exitCode = 2;
        break;
      }

      // Same rationale as set-volume: make single-shot mute reliable even when
      // the PipeWire graph is still being discovered.
      waitForGraph(120);

      const QString mode = args.at(3).trimmed().toLower();
      const PwNodeControls c = graph.nodeControls(*id).value_or(PwNodeControls{});

      bool target = c.mute;
      if (mode == QStringLiteral("on")) {
        target = true;
      } else if (mode == QStringLiteral("off")) {
        target = false;
      } else if (mode == QStringLiteral("toggle")) {
        target = !c.mute;
      } else {
        err << "headroomctl: mute expects on|off|toggle\n";
        exitCode = 2;
        break;
      }

      bool ok = false;
      QElapsedTimer t;
      t.start();
      while (t.elapsed() < 1500) {
        ok = graph.setNodeMute(*id, target);
        if (ok) {
          break;
        }
        waitForGraph(60);
      }
      if (!ok) {
        err << "headroomctl: failed to set mute\n";
        exitCode = 1;
        break;
      }

      // Give PipeWire a short moment to flush the request before exiting.
      waitForGraph(120);

      exitCode = 0;
      break;
    }
  } while (false);

  return exitCode;
}
} // namespace headroomctl

