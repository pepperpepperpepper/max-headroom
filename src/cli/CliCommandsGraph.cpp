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
int handleGraphListingCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err);
int handleGraphControlsCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err);
int handleGraphConnectionsCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err);

bool tryHandleGraphCommands(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut)
{
  bool handled = false;
  int exitCode = 0;
  do {
      if (cmd == QStringLiteral("diagnostics") || cmd == QStringLiteral("nodes") || cmd == QStringLiteral("sinks") || cmd == QStringLiteral("sources")
          || cmd == QStringLiteral("default-sink") || cmd == QStringLiteral("default-source") || cmd == QStringLiteral("ports") || cmd == QStringLiteral("links")
          || cmd == QStringLiteral("set-volume") || cmd == QStringLiteral("mute") || cmd == QStringLiteral("connect") || cmd == QStringLiteral("disconnect")
          || cmd == QStringLiteral("unlink")) {
        handled = true;

        if (cmd == QStringLiteral("diagnostics") || cmd == QStringLiteral("nodes") || cmd == QStringLiteral("sinks") || cmd == QStringLiteral("sources")
            || cmd == QStringLiteral("ports") || cmd == QStringLiteral("links")) {
          exitCode = handleGraphListingCommand(cmd, args, graph, jsonOutput, out, err);
        } else if (cmd == QStringLiteral("default-sink") || cmd == QStringLiteral("default-source") || cmd == QStringLiteral("set-volume") || cmd == QStringLiteral("mute")) {
          exitCode = handleGraphControlsCommand(cmd, args, graph, jsonOutput, out, err);
        } else {
          exitCode = handleGraphConnectionsCommand(cmd, args, graph, jsonOutput, out, err);
        }
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
