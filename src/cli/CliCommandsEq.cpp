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


namespace headroomctl {
bool tryHandleEqCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("eq")) {
        handled = true;
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        if (sub == QStringLiteral("presets")) {
          const auto presets = builtinEqPresets();
          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& p : presets) {
              arr.append(p.first);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& p : presets) {
              out << p.first << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("list")) {
          QList<PwNodeInfo> targets = graph.audioSinks();
          targets.append(graph.audioSources());
          targets.append(graph.audioPlaybackStreams());
          targets.append(graph.audioCaptureStreams());

          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& n : targets) {
              const EqPreset preset = loadEqPresetForNodeName(n.name);
              QJsonObject o = nodeToJson(n);
              o.insert(QStringLiteral("preset"), eqPresetToJson(preset));
              arr.append(o);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& n : targets) {
              const EqPreset preset = loadEqPresetForNodeName(n.name);
              out << n.id << "\t" << n.mediaClass << "\t" << nodeLabel(n) << "\t" << (preset.enabled ? "on" : "off") << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("get")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          if (nodeName.isEmpty()) {
            err << "headroomctl: eq get expects <node-id|node-name>\n";
            exitCode = 2;
            break;
          }

          const EqPreset preset = loadEqPresetForNodeName(nodeName);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("preset"), eqPresetToJson(preset));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\t" << (preset.enabled ? "on" : "off") << "\tpreampDb=" << preset.preampDb << "\tbands=" << preset.bands.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("enable")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          const QString mode = args.at(4).trimmed().toLower();
          if (nodeName.isEmpty()) {
            err << "headroomctl: eq enable expects <node-id|node-name> on|off|toggle\n";
            exitCode = 2;
            break;
          }

          EqPreset preset = loadEqPresetForNodeName(nodeName);
          bool enabled = preset.enabled;
          if (mode == QStringLiteral("on") || mode == QStringLiteral("true") || mode == QStringLiteral("1")) {
            enabled = true;
          } else if (mode == QStringLiteral("off") || mode == QStringLiteral("false") || mode == QStringLiteral("0")) {
            enabled = false;
          } else if (mode == QStringLiteral("toggle")) {
            enabled = !enabled;
          } else {
            err << "headroomctl: eq enable expects on|off|toggle\n";
            exitCode = 2;
            break;
          }

          preset.enabled = enabled;
          saveEqPresetForNodeName(nodeName, preset);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("enabled"), enabled);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\t" << (enabled ? "on" : "off") << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("preset")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          const QString presetName = args.mid(4).join(' ');
          if (nodeName.isEmpty() || presetName.trimmed().isEmpty()) {
            err << "headroomctl: eq preset expects <node-id|node-name> <preset-name>\n";
            exitCode = 2;
            break;
          }

          auto presetOpt = builtinEqPresetByName(presetName);
          if (!presetOpt) {
            err << "headroomctl: unknown preset. Try: headroomctl eq presets\n";
            exitCode = 2;
            break;
          }

          EqPreset preset = *presetOpt;
          preset.enabled = true;
          saveEqPresetForNodeName(nodeName, preset);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("presetName"), presetName);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\tapplied\t" << presetName << "\n";
          }
          exitCode = 0;
          break;
        }

        err << "headroomctl: unknown eq subcommand\n";
        exitCode = 2;
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
