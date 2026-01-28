#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTextStream>

#include <optional>

#include "backend/AlsaSeqBridge.h"
#include "backend/PipeWireThread.h"

#include "cli/CliInternal.h"

namespace headroomctl {
int handleEngineMidiBridgeCommand(QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    const QString bridgeSub = (args.size() >= 4) ? args.at(3).trimmed().toLower() : QStringLiteral("status");

    auto moduleLoadedOpt = [&]() -> std::optional<bool> {
      PipeWireThread pw;
      PipeWireGraph graph(&pw);
      if (!pw.isConnected()) {
        return std::nullopt;
      }
      waitForGraph(200);
      bool loaded = false;
      for (const auto& m : graph.modules()) {
        if (m.name.toLower().contains(QStringLiteral("alsa-seq"))) {
          loaded = true;
          break;
        }
      }
      return loaded;
    };

    if (bridgeSub == QStringLiteral("status")) {
      const bool enabled = AlsaSeqBridge::isConfigInstalled();
      const bool alsaPresent = AlsaSeqBridge::alsaSequencerDevicePresent();
      const auto moduleLoaded = moduleLoadedOpt();

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("configInstalled"), enabled);
        o.insert(QStringLiteral("configPath"), AlsaSeqBridge::configSnippetPath());
        o.insert(QStringLiteral("alsaSequencerDevicePresent"), alsaPresent);
        if (moduleLoaded.has_value()) {
          o.insert(QStringLiteral("pipewireModuleLoaded"), *moduleLoaded);
        } else {
          o.insert(QStringLiteral("pipewireModuleLoaded"), QJsonValue());
        }
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "config\t" << (enabled ? "enabled" : "disabled") << "\n";
        out << "path\t" << AlsaSeqBridge::configSnippetPath() << "\n";
        out << "alsa-seq\t" << (alsaPresent ? "present" : "missing") << "\n";
        if (moduleLoaded.has_value()) {
          out << "module\t" << (*moduleLoaded ? "loaded" : "not-loaded") << "\n";
        } else {
          out << "module\tunknown (PipeWire not reachable)\n";
        }
      }

      exitCode = 0;
      break;
    }

    if (bridgeSub == QStringLiteral("enable")) {
      if (args.size() < 5) {
        err << "headroomctl: engine midi-bridge enable expects on|off|toggle\n";
        exitCode = 2;
        break;
      }

      const QString v = args.at(4).trimmed().toLower();
      const bool cur = AlsaSeqBridge::isConfigInstalled();
      bool target = cur;
      QString opErr;
      bool ok = false;

      if (v == QStringLiteral("on")) {
        target = true;
        ok = AlsaSeqBridge::installConfig(&opErr);
      } else if (v == QStringLiteral("off")) {
        target = false;
        ok = AlsaSeqBridge::removeConfig(&opErr);
      } else if (v == QStringLiteral("toggle")) {
        target = !cur;
        ok = target ? AlsaSeqBridge::installConfig(&opErr) : AlsaSeqBridge::removeConfig(&opErr);
      } else {
        err << "headroomctl: engine midi-bridge enable expects on|off|toggle\n";
        exitCode = 2;
        break;
      }

      if (!ok) {
        err << "headroomctl: failed: " << (opErr.isEmpty() ? QStringLiteral("(unknown)") : opErr) << "\n";
        exitCode = 1;
        break;
      }

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("enabled"), target);
        o.insert(QStringLiteral("configPath"), AlsaSeqBridge::configSnippetPath());
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "ok\tenabled=" << (target ? "1" : "0") << "\n";
        out << "note\trestart PipeWire to apply\n";
      }

      exitCode = 0;
      break;
    }

    err << "headroomctl: engine midi-bridge expects status|enable\n";
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl
