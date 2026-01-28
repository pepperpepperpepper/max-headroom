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
bool tryHandleSessionCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("session") || cmd == QStringLiteral("sessions")) {
        handled = true;
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        QStringList rest = args.mid(2);
        const QString sub = rest.value(0).trimmed().toLower();

        const bool strictLinks = rest.contains(QStringLiteral("--strict-links"));
        if (strictLinks) {
          rest.removeAll(QStringLiteral("--strict-links"));
        }

        const bool mergeSettings = rest.contains(QStringLiteral("--merge-settings"));
        if (mergeSettings) {
          rest.removeAll(QStringLiteral("--merge-settings"));
        }

        const bool strictSettings = !mergeSettings;

        QSettings s;

        if (sub == QStringLiteral("list")) {
          const QStringList names = SessionSnapshotStore::listSnapshotNames(s);
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

        if (sub == QStringLiteral("save")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session save expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const SessionSnapshot snap = snapshotSession(name, graph, s);
          SessionSnapshotStore::save(s, snap);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("name"), snap.name);
            o.insert(QStringLiteral("links"), static_cast<int>(snap.links.size()));
            o.insert(QStringLiteral("eqNodes"), static_cast<int>(snap.eqByNodeName.size()));
            o.insert(QStringLiteral("positions"), static_cast<int>(snap.patchbayPositionByNodeName.size()));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "saved\t" << snap.name << "\tlinks=" << snap.links.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("apply")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session apply expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const auto snap = SessionSnapshotStore::load(s, name);
          if (!snap) {
            err << "headroomctl: session snapshot not found: " << name << "\n";
            exitCode = 1;
            break;
          }

          const SessionSnapshotApplyResult r = applySessionSnapshot(graph, s, *snap, strictLinks, strictSettings);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), r.errors.isEmpty());
            o.insert(QStringLiteral("name"), snap->name);
            o.insert(QStringLiteral("strictLinks"), strictLinks);
            o.insert(QStringLiteral("strictSettings"), strictSettings);
            QJsonObject patchbay;
            patchbay.insert(QStringLiteral("strict"), strictLinks);
            patchbay.insert(QStringLiteral("desiredLinks"), r.patchbay.desiredLinks);
            patchbay.insert(QStringLiteral("createdLinks"), r.patchbay.createdLinks);
            patchbay.insert(QStringLiteral("alreadyPresentLinks"), r.patchbay.alreadyPresentLinks);
            patchbay.insert(QStringLiteral("disconnectedLinks"), r.patchbay.disconnectedLinks);
            patchbay.insert(QStringLiteral("missingEndpoints"), r.patchbay.missingEndpoints);
            o.insert(QStringLiteral("patchbay"), patchbay);
            o.insert(QStringLiteral("defaultSinkRequested"), r.defaultSinkRequested);
            o.insert(QStringLiteral("defaultSinkSet"), r.defaultSinkSet);
            o.insert(QStringLiteral("defaultSourceRequested"), r.defaultSourceRequested);
            o.insert(QStringLiteral("defaultSourceSet"), r.defaultSourceSet);
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
            out << "applied\t" << snap->name << "\tlinks(created=" << r.patchbay.createdLinks << ", already=" << r.patchbay.alreadyPresentLinks
                << ", missing=" << r.patchbay.missingEndpoints << ", errors=" << r.patchbay.errors.size() << ")\n";
            for (const auto& m : r.missing) {
              err << "missing:\t" << m << "\n";
            }
            for (const auto& e : r.errors) {
              err << "error:\t" << e << "\n";
            }
          }
          exitCode = r.errors.isEmpty() ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("delete")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session delete expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const bool removed = SessionSnapshotStore::remove(s, name);
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
  } while (false);

  if (!handled) {
    return false;
  }
  *exitCodeOut = exitCode;
  return true;
}
} // namespace headroomctl
