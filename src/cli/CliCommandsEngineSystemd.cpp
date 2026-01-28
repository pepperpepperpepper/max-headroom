#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <optional>

#include "backend/EngineControl.h"
#include "backend/PipeWireThread.h"

#include "cli/CliInternal.h"

namespace headroomctl {
int handleEngineSystemdCommand(const QString& sub,
                               QStringList args,
                               bool systemctlOk,
                               bool userSystemdOk,
                               const QString& systemdErr,
                               bool jsonOutput,
                               QTextStream& out,
                               QTextStream& err)
{
  int exitCode = 0;
  do {
    auto statusToJson = [](const SystemdUnitStatus& st) {
      QJsonObject o;
      o.insert(QStringLiteral("unit"), st.unit);
      o.insert(QStringLiteral("loadState"), st.loadState);
      o.insert(QStringLiteral("activeState"), st.activeState);
      o.insert(QStringLiteral("subState"), st.subState);
      o.insert(QStringLiteral("description"), st.description);
      o.insert(QStringLiteral("exists"), st.exists());
      o.insert(QStringLiteral("active"), st.isActive());
      if (!st.error.isEmpty()) {
        o.insert(QStringLiteral("error"), st.error);
      }
      return o;
    };

    if (sub == QStringLiteral("status")) {
      const QStringList units = EngineControl::defaultUserUnits();
      QVector<SystemdUnitStatus> statuses;
      if (userSystemdOk) {
        statuses.reserve(units.size());
        for (const auto& u : units) {
          statuses.push_back(EngineControl::queryUserUnit(u));
        }
      }

      PipeWireThread pw;
      PipeWireGraph graph(&pw);
      const bool pipewireOk = pw.isConnected();
      int nodeCount = 0;
      int sinkCount = 0;
      int sourceCount = 0;
      int playbackStreamCount = 0;
      int captureStreamCount = 0;
      bool hasProfiler = false;
      bool hasClock = false;

      if (pipewireOk) {
        waitForGraph(200);
        nodeCount = graph.nodes().size();
        sinkCount = graph.audioSinks().size();
        sourceCount = graph.audioSources().size();
        playbackStreamCount = graph.audioPlaybackStreams().size();
        captureStreamCount = graph.audioCaptureStreams().size();
        hasProfiler = graph.hasProfilerSupport();
        hasClock = graph.hasClockSettingsSupport();
      }

      const auto pipewireProc = processRunningExact(QStringLiteral("pipewire"));
      const auto wireplumberProc = processRunningExact(QStringLiteral("wireplumber"));
      const auto pipewirePulseProc = processRunningExact(QStringLiteral("pipewire-pulse"));

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), userSystemdOk);
        o.insert(QStringLiteral("systemctlAvailable"), systemctlOk);
        o.insert(QStringLiteral("userSystemdAvailable"), userSystemdOk);
        if (!systemdErr.isEmpty()) {
          o.insert(QStringLiteral("error"), systemdErr);
        }
        o.insert(QStringLiteral("pipewireReachable"), pipewireOk);
        QJsonObject pwObj;
        pwObj.insert(QStringLiteral("reachable"), pipewireOk);
        if (pipewireOk) {
          pwObj.insert(QStringLiteral("nodes"), nodeCount);
          pwObj.insert(QStringLiteral("sinks"), sinkCount);
          pwObj.insert(QStringLiteral("sources"), sourceCount);
          pwObj.insert(QStringLiteral("playbackStreams"), playbackStreamCount);
          pwObj.insert(QStringLiteral("captureStreams"), captureStreamCount);
          pwObj.insert(QStringLiteral("profiler"), hasProfiler);
          pwObj.insert(QStringLiteral("clock"), hasClock);
        }
        o.insert(QStringLiteral("pipewire"), pwObj);

        QJsonObject procs;
        procs.insert(QStringLiteral("pipewire"), pipewireProc.has_value() ? QJsonValue(*pipewireProc) : QJsonValue());
        procs.insert(QStringLiteral("wireplumber"), wireplumberProc.has_value() ? QJsonValue(*wireplumberProc) : QJsonValue());
        procs.insert(QStringLiteral("pipewirePulse"), pipewirePulseProc.has_value() ? QJsonValue(*pipewirePulseProc) : QJsonValue());
        o.insert(QStringLiteral("processes"), procs);

        QJsonArray arr;
        for (const auto& st : statuses) {
          arr.append(statusToJson(st));
        }
        o.insert(QStringLiteral("units"), arr);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "systemctl\t" << (systemctlOk ? "available" : "missing") << "\n";
        if (systemctlOk) {
          out << "systemd-user\t" << (userSystemdOk ? "available" : "unavailable");
          if (!userSystemdOk && !systemdErr.isEmpty()) {
            out << "\t" << systemdErr;
          }
          out << "\n";
        }
        out << "pipewire\t" << (pipewireOk ? "reachable" : "unreachable") << "\n";
        if (pipewireOk) {
          out << "nodes\t" << nodeCount << "\n";
          out << "sinks\t" << sinkCount << "\n";
          out << "sources\t" << sourceCount << "\n";
          out << "streams-out\t" << playbackStreamCount << "\n";
          out << "streams-in\t" << captureStreamCount << "\n";
          out << "profiler\t" << (hasProfiler ? "available" : "unavailable") << "\n";
          out << "clock\t" << (hasClock ? "available" : "unavailable") << "\n";
        }

        auto fmtProc = [](const std::optional<bool>& v) -> QString {
          if (!v.has_value()) {
            return QStringLiteral("unknown");
          }
          return *v ? QStringLiteral("running") : QStringLiteral("not-running");
        };
        out << "proc-pipewire\t" << fmtProc(pipewireProc) << "\n";
        out << "proc-wireplumber\t" << fmtProc(wireplumberProc) << "\n";
        out << "proc-pipewire-pulse\t" << fmtProc(pipewirePulseProc) << "\n";

        if (userSystemdOk) {
          for (const auto& st : statuses) {
            out << st.unit << "\t" << (st.exists() ? st.loadState : QStringLiteral("not-found")) << "\t" << st.activeState << "\t" << st.subState << "\t"
                << st.description << "\n";
          }
        }
      }

      exitCode = 0;
      break;
    }

    if (sub == QStringLiteral("start") || sub == QStringLiteral("stop") || sub == QStringLiteral("restart")) {
      if (args.size() < 4) {
        err << "headroomctl: engine " << sub << " expects <unit|all>\n";
        exitCode = 2;
        break;
      }
      if (!systemctlOk) {
        err << "headroomctl: systemctl not found\n";
        exitCode = 1;
        break;
      }
      if (!userSystemdOk) {
        err << "headroomctl: systemd user instance unavailable: " << systemdErr << "\n";
        exitCode = 1;
        break;
      }

      const QString target = args.at(3).trimmed();
      QStringList units;
      if (target.toLower() == QStringLiteral("all")) {
        units = EngineControl::defaultUserUnits();
      } else {
        units = QStringList{EngineControl::normalizeUnitAlias(target)};
      }

      bool allOk = true;
      QJsonArray results;
      QStringList lines;

      for (const auto& u : units) {
        QString actionErr;
        bool ok = false;
        if (sub == QStringLiteral("start")) {
          ok = EngineControl::startUserUnit(u, &actionErr);
        } else if (sub == QStringLiteral("stop")) {
          ok = EngineControl::stopUserUnit(u, &actionErr);
        } else if (sub == QStringLiteral("restart")) {
          ok = EngineControl::restartUserUnit(u, &actionErr);
        }

        const SystemdUnitStatus after = EngineControl::queryUserUnit(u);

        if (jsonOutput) {
          QJsonObject r;
          r.insert(QStringLiteral("unit"), u);
          r.insert(QStringLiteral("action"), sub);
          r.insert(QStringLiteral("ok"), ok);
          if (!actionErr.isEmpty()) {
            r.insert(QStringLiteral("error"), actionErr);
          }
          r.insert(QStringLiteral("status"), statusToJson(after));
          results.append(r);
        } else {
          QString line = QStringLiteral("%1\t%2").arg(u, ok ? QStringLiteral("ok") : QStringLiteral("failed"));
          if (!actionErr.isEmpty()) {
            line += QStringLiteral("\t%1").arg(actionErr);
          }
          lines.push_back(line);
        }

        if (!ok) {
          allOk = false;
        }
      }

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), allOk);
        o.insert(QStringLiteral("results"), results);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        for (const auto& l : lines) {
          out << l << "\n";
        }
      }

      exitCode = allOk ? 0 : 1;
      break;
    }

    err << "headroomctl: engine expects status|start|stop|restart|midi-bridge|clock\n";
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl
