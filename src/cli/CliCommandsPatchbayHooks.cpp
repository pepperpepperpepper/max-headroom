#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTextStream>

#include "backend/PatchbayProfileHooks.h"
#include "cli/CliInternal.h"

namespace headroomctl {
int handlePatchbayHooksSubcommand(QStringList args, QSettings& s, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    if (args.size() < 5) {
      printUsage(err);
      exitCode = 2;
      break;
    }

    const QString action = args.at(3).trimmed().toLower();
    const QString profileName = args.at(4).trimmed();
    if (profileName.isEmpty()) {
      err << "headroomctl: patchbay hooks expects <profile-name>\n";
      exitCode = 2;
      break;
    }

    if (action == QStringLiteral("get")) {
      const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("name"), profileName);
        o.insert(QStringLiteral("onLoad"), h.onLoadCommand);
        o.insert(QStringLiteral("onUnload"), h.onUnloadCommand);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "profile\t" << profileName << "\n";
        out << "on-load\t" << h.onLoadCommand << "\n";
        out << "on-unload\t" << h.onUnloadCommand << "\n";
      }
      exitCode = 0;
      break;
    }

    if (action == QStringLiteral("set")) {
      if (args.size() < 7) {
        err << "headroomctl: patchbay hooks set expects <profile-name> load|unload <command...>\n";
        exitCode = 2;
        break;
      }
      const QString which = args.at(5).trimmed().toLower();
      const QString cmdText = args.mid(6).join(QLatin1Char(' ')).trimmed();

      PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
      if (which == QStringLiteral("load") || which == QStringLiteral("onload")) {
        h.onLoadCommand = cmdText;
      } else if (which == QStringLiteral("unload") || which == QStringLiteral("onunload")) {
        h.onUnloadCommand = cmdText;
      } else {
        err << "headroomctl: patchbay hooks set expects load|unload\n";
        exitCode = 2;
        break;
      }

      PatchbayProfileHooksStore::save(s, profileName, h);
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("name"), profileName);
        o.insert(QStringLiteral("onLoad"), h.onLoadCommand);
        o.insert(QStringLiteral("onUnload"), h.onUnloadCommand);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "set\t" << which << "\t" << profileName << "\n";
      }
      exitCode = 0;
      break;
    }

    if (action == QStringLiteral("clear")) {
      if (args.size() < 6) {
        err << "headroomctl: patchbay hooks clear expects <profile-name> load|unload|all\n";
        exitCode = 2;
        break;
      }
      const QString which = args.at(5).trimmed().toLower();
      if (which == QStringLiteral("all")) {
        PatchbayProfileHooksStore::clear(s, profileName);
      } else {
        PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
        if (which == QStringLiteral("load") || which == QStringLiteral("onload")) {
          h.onLoadCommand.clear();
        } else if (which == QStringLiteral("unload") || which == QStringLiteral("onunload")) {
          h.onUnloadCommand.clear();
        } else {
          err << "headroomctl: patchbay hooks clear expects load|unload|all\n";
          exitCode = 2;
          break;
        }
        PatchbayProfileHooksStore::save(s, profileName, h);
      }

      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("name"), profileName);
        o.insert(QStringLiteral("cleared"), which);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "cleared\t" << which << "\t" << profileName << "\n";
      }
      exitCode = 0;
      break;
    }

    err << "headroomctl: patchbay hooks expects get|set|clear\n";
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl

