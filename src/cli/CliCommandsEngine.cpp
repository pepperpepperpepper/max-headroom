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
int handleEngineClockCommand(QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err);
int handleEngineMidiBridgeCommand(QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err);
int handleEngineSystemdCommand(const QString& sub,
                               QStringList args,
                               bool systemctlOk,
                               bool userSystemdOk,
                               const QString& systemdErr,
                               bool jsonOutput,
                               QTextStream& out,
                               QTextStream& err);

bool tryHandleEngineCommand(const QString& cmd, QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("engine")) {
        handled = true;
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        const bool systemctlOk = EngineControl::isSystemctlAvailable();
        QString systemdErr;
        const bool userSystemdOk = systemctlOk ? EngineControl::canTalkToUserSystemd(&systemdErr) : false;

        if (sub == QStringLiteral("midi-bridge")) {
          exitCode = handleEngineMidiBridgeCommand(args, jsonOutput, out, err);
          break;
        }

        if (sub == QStringLiteral("clock")) {
          exitCode = handleEngineClockCommand(args, jsonOutput, out, err);
          break;
        }

        if (sub == QStringLiteral("status") || sub == QStringLiteral("start") || sub == QStringLiteral("stop") || sub == QStringLiteral("restart")) {
          exitCode = handleEngineSystemdCommand(sub, args, systemctlOk, userSystemdOk, systemdErr, jsonOutput, out, err);
          break;
        }

        err << "headroomctl: engine expects status|start|stop|restart|midi-bridge|clock\n";
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
