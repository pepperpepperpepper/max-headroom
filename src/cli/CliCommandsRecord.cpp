#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPair>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <limits>
#include <optional>

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "backend/AudioRecorder.h"
#include "backend/AlsaSeqBridge.h"
#include "backend/EngineControl.h"
#include "backend/PatchbayProfiles.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PatchbayPortConfig.h"
#include "backend/PatchbayAutoConnectRules.h"
#include "backend/SessionSnapshots.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "backend/EqConfig.h"
#include "settings/SettingsKeys.h"

#include "cli/CliInternal.h"


namespace {

std::atomic_bool g_recordStopRequested{false};

extern "C" void onRecordSignal(int)
{
  g_recordStopRequested.store(true);
}
} // namespace

namespace headroomctl {
bool tryHandleRecordStatusStop(const QString& cmd, QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  if (cmd != QStringLiteral("record")) {
    return false;
  }
  if (args.size() < 3) {
    printUsage(err);
    *exitCodeOut = 2;
    return true;
  }

  const QString sub = args.at(2).trimmed().toLower();
  const QString statusPath = recordingStatusPath();

  if (sub == QStringLiteral("status")) {
    QString readErr;
    const auto obj = readJsonObjectFile(statusPath, &readErr);
    if (!obj) {
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("recording"), false);
        o.insert(QStringLiteral("statusPath"), statusPath);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "not-recording\n";
      }
      *exitCodeOut = 0;
      return true;
    }

    const qint64 pid = obj->value(QStringLiteral("pid")).toVariant().toLongLong();
    const bool running = pidAlive(pid) && obj->value(QStringLiteral("running")).toBool(true);

    if (jsonOutput) {
      QJsonObject o = *obj;
      o.insert(QStringLiteral("recording"), running);
      o.insert(QStringLiteral("statusPath"), statusPath);
      out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
    } else {
      const QString fmt = obj->value(QStringLiteral("format")).toString(QStringLiteral("wav"));
      const double dur = obj->value(QStringLiteral("durationSeconds")).toDouble(0.0);
      const double peak = obj->value(QStringLiteral("peakDb")).toDouble(std::numeric_limits<double>::quiet_NaN());
      const double rms = obj->value(QStringLiteral("rmsDb")).toDouble(std::numeric_limits<double>::quiet_NaN());
      out << (running ? "recording" : "stopped") << "\tpid=" << pid << "\tfile="
          << obj->value(QStringLiteral("filePath")).toString() << "\tfmt=" << fmt
          << "\tdur=" << QString::number(dur, 'f', 1) << "s"
          << "\tpeak=" << (std::isfinite(peak) ? QString::number(peak, 'f', 1) + "dB" : QStringLiteral("-"))
          << "\trms=" << (std::isfinite(rms) ? QString::number(rms, 'f', 1) + "dB" : QStringLiteral("-"))
          << "\tbytes=" << obj->value(QStringLiteral("bytesWritten")).toVariant().toLongLong()
          << "\tstate=" << obj->value(QStringLiteral("streamState")).toString() << "\n";
    }

    *exitCodeOut = 0;
    return true;
  }

  if (sub == QStringLiteral("stop")) {
    QString readErr;
    const auto obj = readJsonObjectFile(statusPath, &readErr);
    if (!obj) {
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("alreadyStopped"), true);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "not-recording\n";
      }
      *exitCodeOut = 0;
      return true;
    }

    const qint64 pid = obj->value(QStringLiteral("pid")).toVariant().toLongLong();
    const bool ok = stopPid(pid, 2000, err);
    if (!ok) {
      *exitCodeOut = 1;
      return true;
    }

    if (!pidAlive(pid)) {
      QFile::remove(statusPath);
    }

    if (jsonOutput) {
      QJsonObject o;
      o.insert(QStringLiteral("ok"), true);
      o.insert(QStringLiteral("pid"), pid);
      out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
    } else {
      out << "stopped\tpid=" << pid << "\n";
    }

    *exitCodeOut = 0;
    return true;
  }

  return false;
}

bool tryHandleRecordPipeWire(QCoreApplication& app, const QString& cmd, QStringList args, PipeWireThread& pw, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("record")) {
        handled = true;
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        if (sub == QStringLiteral("targets")) {
          auto streamLabel = [&](const PwNodeInfo& n) -> QString {
            const QString base = nodeLabel(n);
            const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
            return (!app.isEmpty() && app != base) ? QStringLiteral("%1 — %2").arg(app, base) : base;
          };

          struct Target final {
            QString kind;
            QString label;
            std::optional<uint32_t> nodeId;
            QString targetObject;
            bool captureSink = true;
          };

          QVector<Target> targets;
          targets.push_back({QStringLiteral("system-mix"), QStringLiteral("System mix (default output monitor)"), std::nullopt, QString{}, true});

          for (const auto& s : graph.audioSinks()) {
            targets.push_back({QStringLiteral("output-device"), QStringLiteral("Output: %1").arg(nodeLabel(s)), s.id, s.name, true});
          }
          for (const auto& s : graph.audioPlaybackStreams()) {
            targets.push_back({QStringLiteral("app-playback"), QStringLiteral("App playback: %1").arg(streamLabel(s)), s.id, s.name, false});
          }

          targets.push_back({QStringLiteral("default-input"), QStringLiteral("Default input (mic)"), std::nullopt, QString{}, false});

          for (const auto& s : graph.audioSources()) {
            targets.push_back({QStringLiteral("input-device"), QStringLiteral("Input: %1").arg(nodeLabel(s)), s.id, s.name, false});
          }
          for (const auto& s : graph.audioCaptureStreams()) {
            targets.push_back({QStringLiteral("app-recording"), QStringLiteral("App recording: %1").arg(streamLabel(s)), s.id, s.name, true});
          }

          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& t : targets) {
              QJsonObject o;
              o.insert(QStringLiteral("kind"), t.kind);
              o.insert(QStringLiteral("label"), t.label);
              o.insert(QStringLiteral("targetObject"), t.targetObject);
              o.insert(QStringLiteral("captureSink"), t.captureSink);
              if (t.nodeId) {
                o.insert(QStringLiteral("nodeId"), static_cast<qint64>(*t.nodeId));
              }
              arr.append(o);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "kind\tcapture\tid\ttargetObject\tlabel\n";
            for (const auto& t : targets) {
              out << t.kind << "\t" << (t.captureSink ? "sink" : "source") << "\t" << (t.nodeId ? QString::number(*t.nodeId) : "-")
                  << "\t" << (t.targetObject.isEmpty() ? "-" : t.targetObject) << "\t" << t.label << "\n";
            }
          }

          exitCode = 0;
          break;
        }

        if (args.size() < 4) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString filePath = args.at(3).trimmed();

        QString targetArg;
        std::optional<AudioRecorder::Format> formatOpt;
        int durationSec = 0;
        bool captureSink = true;
        bool background = false;

        for (int i = 4; i < args.size(); ++i) {
          const QString a = args.at(i).trimmed();
          if (a == QStringLiteral("--target")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --target requires a value\n";
              exitCode = 2;
              break;
            }
            targetArg = args.at(i + 1);
            ++i;
            continue;
          }
          if (a == QStringLiteral("--format")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --format requires a value\n";
              exitCode = 2;
              break;
            }
            const auto f = AudioRecorder::formatFromString(args.at(i + 1));
            if (!f) {
              err << "headroomctl: invalid --format (expected wav|flac)\n";
              exitCode = 2;
              break;
            }
            formatOpt = *f;
            ++i;
            continue;
          }
          if (a == QStringLiteral("--duration") || a == QStringLiteral("--seconds")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --duration requires a value\n";
              exitCode = 2;
              break;
            }
            bool ok = false;
            const int v = args.at(i + 1).trimmed().toInt(&ok);
            if (!ok || v < 0) {
              err << "headroomctl: invalid --duration (expected seconds >= 0)\n";
              exitCode = 2;
              break;
            }
            durationSec = v;
            ++i;
            continue;
          }
          if (a == QStringLiteral("--sink")) {
            captureSink = true;
            continue;
          }
          if (a == QStringLiteral("--source")) {
            captureSink = false;
            continue;
          }
          if (a == QStringLiteral("--background") || a == QStringLiteral("--daemon")) {
            background = true;
            continue;
          }
          err << "headroomctl: unknown record option: " << a << "\n";
          exitCode = 2;
          break;
        }
        if (exitCode != 0) {
          break;
        }

        auto inferFormatFromPath = [&](const QString& p) -> AudioRecorder::Format {
          const QString lower = p.trimmed().toLower();
          if (lower.endsWith(QStringLiteral(".flac"))) {
            return AudioRecorder::Format::Flac;
          }
          return AudioRecorder::Format::Wav;
        };
        const AudioRecorder::Format format = formatOpt.value_or(inferFormatFromPath(filePath));

        if (sub == QStringLiteral("start")) {
          if (background) {
            QStringList daemonArgs;
            daemonArgs << QStringLiteral("record") << QStringLiteral("run") << filePath;
            if (!targetArg.trimmed().isEmpty()) {
              daemonArgs << QStringLiteral("--target") << targetArg;
            }
            daemonArgs << QStringLiteral("--format") << AudioRecorder::formatToString(format);
            daemonArgs << QStringLiteral("--duration") << QString::number(durationSec);
            daemonArgs << (captureSink ? QStringLiteral("--sink") : QStringLiteral("--source"));

            qint64 pid = 0;
            const bool ok = QProcess::startDetached(QCoreApplication::applicationFilePath(), daemonArgs, QString{}, &pid);
            if (!ok) {
              err << "headroomctl: failed to start detached recorder\n";
              exitCode = 1;
              break;
            }
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("pid"), pid);
              o.insert(QStringLiteral("statusPath"), recordingStatusPath());
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "started\tpid=" << pid << "\n";
            }
            exitCode = 0;
            break;
          }

          // Foreground start = run.
        }

        if (sub != QStringLiteral("run") && sub != QStringLiteral("start")) {
          err << "headroomctl: record expects start|stop|status\n";
          exitCode = 2;
          break;
        }

        bool okId = false;
        const uint32_t nodeId = targetArg.trimmed().toUInt(&okId);
        QString targetObject = targetArg.trimmed();
        if (okId) {
          const auto n = graph.nodeById(nodeId);
          if (!n) {
            err << "headroomctl: unknown node id " << nodeId << "\n";
            exitCode = 2;
            break;
          }
          targetObject = n->name;
        }

        const QString statusPath = recordingStatusPath();
        if (const auto existing = readJsonObjectFile(statusPath, nullptr)) {
          const qint64 pid = existing->value(QStringLiteral("pid")).toVariant().toLongLong();
          const bool running = pidAlive(pid) && existing->value(QStringLiteral("running")).toBool(true);
          if (running) {
            err << "headroomctl: already recording (pid " << pid << "). Stop first.\n";
            exitCode = 1;
            break;
          }
        }

        g_recordStopRequested.store(false);
        std::signal(SIGINT, onRecordSignal);
        std::signal(SIGTERM, onRecordSignal);

        QString targetLabel;
        if (targetObject.isEmpty()) {
          targetLabel = captureSink ? QStringLiteral("system-mix") : QStringLiteral("default-input");
        } else {
          targetLabel = targetObject;
          for (const auto& n : graph.nodes()) {
            if (n.name == targetObject) {
              targetLabel = nodeLabel(n);
              break;
            }
          }
        }

        const QString resolvedPath = AudioRecorder::expandPathTemplate(filePath, targetLabel, format);

        AudioRecorder recorder(&pw);
        AudioRecorder::StartOptions recOpts;
        recOpts.filePath = resolvedPath;
        recOpts.targetObject = targetObject;
        recOpts.captureSink = captureSink;
        recOpts.format = format;
        if (!recorder.start(recOpts)) {
          const QString e = recorder.lastError();
          err << "headroomctl: record start failed" << (e.isEmpty() ? "" : ": ") << e << "\n";
          exitCode = 1;
          break;
        }

        const qint64 pid = QCoreApplication::applicationPid();
        const QString startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        QJsonObject state;
        state.insert(QStringLiteral("pid"), pid);
        state.insert(QStringLiteral("running"), true);
        state.insert(QStringLiteral("startedAt"), startedAt);
        state.insert(QStringLiteral("filePathTemplate"), filePath);
        state.insert(QStringLiteral("filePath"), recorder.filePath());
        state.insert(QStringLiteral("format"), AudioRecorder::formatToString(format));
        state.insert(QStringLiteral("targetObject"), targetObject);
        state.insert(QStringLiteral("targetLabel"), targetLabel);
        state.insert(QStringLiteral("captureSink"), captureSink);
        if (durationSec > 0) {
          state.insert(QStringLiteral("durationLimitSeconds"), durationSec);
        }

        if (!graph.nodes().isEmpty()) {
          auto snapshotList = [&](const QList<PwNodeInfo>& list) {
            QJsonArray arr;
            for (const auto& n : list) {
              arr.append(nodeToJson(n));
            }
            return arr;
          };

          QJsonObject snapshot;
          snapshot.insert(QStringLiteral("sinks"), snapshotList(graph.audioSinks()));
          snapshot.insert(QStringLiteral("sources"), snapshotList(graph.audioSources()));
          snapshot.insert(QStringLiteral("playbackStreams"), snapshotList(graph.audioPlaybackStreams()));
          snapshot.insert(QStringLiteral("captureStreams"), snapshotList(graph.audioCaptureStreams()));

          if (const auto defSink = graph.defaultAudioSinkId()) {
            snapshot.insert(QStringLiteral("defaultAudioSinkId"), static_cast<qint64>(*defSink));
          }
          if (const auto defSource = graph.defaultAudioSourceId()) {
            snapshot.insert(QStringLiteral("defaultAudioSourceId"), static_cast<qint64>(*defSource));
          }

          if (!targetObject.isEmpty()) {
            for (const auto& n : graph.nodes()) {
              if (n.name == targetObject) {
                snapshot.insert(QStringLiteral("targetNodeId"), static_cast<qint64>(n.id));
                snapshot.insert(QStringLiteral("targetMediaClass"), n.mediaClass);
                break;
              }
            }
          }

          state.insert(QStringLiteral("graphSnapshot"), snapshot);
        }

        auto writeStatus = [&]() {
          const uint32_t sr = recorder.sampleRate();
          const uint64_t frames = recorder.framesCaptured();
          const double dur = (sr > 0) ? (static_cast<double>(frames) / static_cast<double>(sr)) : 0.0;

          state.insert(QStringLiteral("sampleRate"), static_cast<int>(sr));
          state.insert(QStringLiteral("channels"), static_cast<int>(recorder.channels()));
          state.insert(QStringLiteral("quantumFrames"), static_cast<int>(recorder.quantumFrames()));
          state.insert(QStringLiteral("lastBufferFrames"), static_cast<int>(recorder.lastBufferFrames()));
          state.insert(QStringLiteral("lastBufferBytes"), static_cast<int>(recorder.lastBufferBytes()));
          state.insert(QStringLiteral("bytesWritten"), static_cast<qint64>(recorder.dataBytesWritten()));
          state.insert(QStringLiteral("framesCaptured"), static_cast<qint64>(frames));
          state.insert(QStringLiteral("durationSeconds"), dur);
          state.insert(QStringLiteral("peakDb"), recorder.peakDb());
          state.insert(QStringLiteral("rmsDb"), recorder.rmsDb());
          state.insert(QStringLiteral("streamState"), recorder.streamStateString());
          const QString le = recorder.lastError();
          if (!le.isEmpty()) {
            state.insert(QStringLiteral("lastError"), le);
          }

          QString writeErr;
          (void)writeJsonFileAtomic(statusPath, state, &writeErr);
        };

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &recorder, [&]() {
          recorder.stop();
          state.insert(QStringLiteral("running"), false);
          state.insert(QStringLiteral("stoppedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
          writeStatus();
        });

        QTimer statusTimer;
        statusTimer.setInterval(500);
        QObject::connect(&statusTimer, &QTimer::timeout, &app, [&]() { writeStatus(); });
        statusTimer.start();

        QTimer stopTimer;
        stopTimer.setInterval(100);
        QObject::connect(&stopTimer, &QTimer::timeout, &app, [&]() {
          if (g_recordStopRequested.load()) {
            app.quit();
          }
        });
        stopTimer.start();

        QTimer durationTimer;
        if (durationSec > 0) {
          durationTimer.setSingleShot(true);
          durationTimer.setInterval(durationSec * 1000);
          QObject::connect(&durationTimer, &QTimer::timeout, &app, [&]() { app.quit(); });
          durationTimer.start();
        }

        writeStatus();

        if (sub == QStringLiteral("start")) {
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("pid"), pid);
            o.insert(QStringLiteral("statusPath"), statusPath);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "recording\tpid=" << pid << "\t(ctrl-c to stop; or headroomctl record stop)\n";
          }
        }

        exitCode = app.exec();
        break;
      }
  } while (false);

  if (!handled) {
    return false;
  }
  *exitCodeOut = exitCode;
  return true;
}
} // namespace headroomctl
