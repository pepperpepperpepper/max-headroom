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
int handlePatchbayHooksSubcommand(QStringList args, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err);
int handlePatchbayPortConfigSubcommand(QStringList args, PipeWireGraph& graph, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err);
int handlePatchbayAutoconnectSubcommand(QStringList args, PipeWireGraph& graph, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err);

bool tryHandlePatchbayCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("patchbay")) {
        handled = true;
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        const bool strict = args.contains(QStringLiteral("--strict"));
        if (strict) {
          args.removeAll(QStringLiteral("--strict"));
        }
        const bool noHooks = args.contains(QStringLiteral("--no-hooks"));
        if (noHooks) {
          args.removeAll(QStringLiteral("--no-hooks"));
        }

        QSettings s;

        if (sub == QStringLiteral("profiles")) {
          const QStringList names = PatchbayProfileStore::listProfileNames(s);
          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& name : names) {
              arr.append(name);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& name : names) {
              out << name << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("hooks")) {
          exitCode = handlePatchbayHooksSubcommand(args, s, jsonOutput, out, err);
          break;
        }

        if (sub == QStringLiteral("save")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay save expects <profile-name>\n";
            exitCode = 2;
            break;
          }

          const PatchbayProfile profile = snapshotPatchbayProfile(name, graph);
          PatchbayProfileStore::save(s, profile);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("name"), profile.name);
            o.insert(QStringLiteral("links"), static_cast<int>(profile.links.size()));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "saved\t" << profile.name << "\tlinks=" << profile.links.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("apply")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay apply expects <profile-name>\n";
            exitCode = 2;
            break;
          }

          const QString prevActive = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();

          const auto profile = PatchbayProfileStore::load(s, name);
          if (!profile) {
            err << "headroomctl: patchbay profile not found: " << name << "\n";
            exitCode = 1;
            break;
          }

          PatchbayProfileHookStartResult unloadHook;
          if (!noHooks && !prevActive.isEmpty() && prevActive != profile->name) {
            const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, prevActive);
            unloadHook = startPatchbayProfileHookDetached(prevActive,
                                                         QString{},
                                                         profile->name,
                                                         PatchbayProfileHookEvent::Unload,
                                                         h.onUnloadCommand);
          }

          const PatchbayProfileApplyResult r = applyPatchbayProfile(graph, *profile, strict);

          PatchbayProfileHookStartResult loadHook;
          if (!noHooks) {
            const PatchbayProfileHooks h2 = PatchbayProfileHooksStore::load(s, profile->name);
            loadHook = startPatchbayProfileHookDetached(profile->name,
                                                       prevActive,
                                                       QString{},
                                                       PatchbayProfileHookEvent::Load,
                                                       h2.onLoadCommand);
          }

          s.setValue(SettingsKeys::patchbaySelectedProfileName(), profile->name);
          s.setValue(SettingsKeys::patchbayActiveProfileName(), profile->name);

          if (jsonOutput) {
            QJsonObject o;
            const bool hookOk = unloadHook.error.isEmpty() && loadHook.error.isEmpty();
            o.insert(QStringLiteral("ok"), r.errors.isEmpty() && hookOk);
            o.insert(QStringLiteral("name"), profile->name);
            o.insert(QStringLiteral("strict"), strict);
            o.insert(QStringLiteral("hooksEnabled"), !noHooks);
            o.insert(QStringLiteral("prevActiveProfile"), prevActive);
            o.insert(QStringLiteral("desiredLinks"), r.desiredLinks);
            o.insert(QStringLiteral("createdLinks"), r.createdLinks);
            o.insert(QStringLiteral("alreadyPresentLinks"), r.alreadyPresentLinks);
            o.insert(QStringLiteral("disconnectedLinks"), r.disconnectedLinks);
            o.insert(QStringLiteral("missingEndpoints"), r.missingEndpoints);
            QJsonObject hooks;
            hooks.insert(QStringLiteral("unloadStarted"), unloadHook.started);
            hooks.insert(QStringLiteral("unloadPid"), static_cast<qint64>(unloadHook.pid));
            hooks.insert(QStringLiteral("unloadError"), unloadHook.error);
            hooks.insert(QStringLiteral("loadStarted"), loadHook.started);
            hooks.insert(QStringLiteral("loadPid"), static_cast<qint64>(loadHook.pid));
            hooks.insert(QStringLiteral("loadError"), loadHook.error);
            o.insert(QStringLiteral("hooks"), hooks);
            QJsonArray missing;
            for (const auto& m : r.missing) {
              missing.append(m);
            }
            QJsonArray errors;
            for (const auto& e : r.errors) {
              errors.append(e);
            }
            o.insert(QStringLiteral("missing"), missing);
            o.insert(QStringLiteral("errors"), errors);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "applied\t" << profile->name << "\tcreated=" << r.createdLinks << "\talready=" << r.alreadyPresentLinks;
            if (strict) {
              out << "\tdisconnected=" << r.disconnectedLinks;
            }
            out << "\tmissing=" << r.missingEndpoints << "\terrors=" << r.errors.size() << "\n";
            for (const auto& e : r.errors) {
              err << "error:\t" << e << "\n";
            }
            if (!noHooks) {
              if (unloadHook.started) {
                out << "hook\tunload\tpid=" << unloadHook.pid << "\tprofile=" << prevActive << "\n";
              } else if (!unloadHook.error.isEmpty()) {
                err << "hook-error:\tunload\t" << unloadHook.error << "\tprofile=" << prevActive << "\n";
              }
              if (loadHook.started) {
                out << "hook\tload\tpid=" << loadHook.pid << "\tprofile=" << profile->name << "\n";
              } else if (!loadHook.error.isEmpty()) {
                err << "hook-error:\tload\t" << loadHook.error << "\tprofile=" << profile->name << "\n";
              }
            }
          }
          exitCode = (r.errors.isEmpty() && unloadHook.error.isEmpty() && loadHook.error.isEmpty()) ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("delete")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay delete expects <profile-name>\n";
            exitCode = 2;
            break;
          }
          const bool removed = PatchbayProfileStore::remove(s, name);
          if (removed) {
            PatchbayProfileHooksStore::clear(s, name);
            if (s.value(SettingsKeys::patchbaySelectedProfileName()).toString().trimmed() == name) {
              s.remove(SettingsKeys::patchbaySelectedProfileName());
            }
            if (s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed() == name) {
              s.remove(SettingsKeys::patchbayActiveProfileName());
            }
          }
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

        if (sub == QStringLiteral("port")) {
          exitCode = handlePatchbayPortConfigSubcommand(args, graph, s, jsonOutput, out, err);
          break;
        }

        if (sub == QStringLiteral("autoconnect")) {
          exitCode = handlePatchbayAutoconnectSubcommand(args, graph, s, jsonOutput, out, err);
          break;
        }

        printUsage(err);
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
