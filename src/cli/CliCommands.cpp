#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>

#include "backend/PipeWireThread.h"
#include "cli/CliInternal.h"

namespace headroomctl {
bool tryHandleEngineCommand(const QString& cmd, QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandleRecordStatusStop(const QString& cmd, QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandleGraphCommands(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandlePatchbayCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandleSessionCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandleEqCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err, int* exitCodeOut);
bool tryHandleRecordPipeWire(QCoreApplication& app,
                             const QString& cmd,
                             QStringList args,
                             PipeWireThread& pw,
                             PipeWireGraph& graph,
                             bool jsonOutput,
                             QTextStream& out,
                             QTextStream& err,
                             int* exitCodeOut);

int runCommand(QCoreApplication& app, QStringList args, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;

  do {
    if (args.contains(QStringLiteral("--version")) || args.contains(QStringLiteral("-V"))) {
      out << QCoreApplication::applicationVersion() << "\n";
      exitCode = 0;
      break;
    }
    if (args.size() < 2) {
      printUsage(out);
      exitCode = 2;
      break;
    }

    const bool jsonOutput = args.contains(QStringLiteral("--json"));
    if (jsonOutput) {
      args.removeAll(QStringLiteral("--json"));
    }
    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h")) || args.contains(QStringLiteral("help"))) {
      printUsage(out);
      exitCode = 0;
      break;
    }

    const QString cmd = args.at(1).trimmed().toLower();
    if (cmd == QStringLiteral("version")) {
      out << QCoreApplication::applicationVersion() << "\n";
      exitCode = 0;
      break;
    }

    if (tryHandleRecordStatusStop(cmd, args, jsonOutput, out, err, &exitCode)) {
      break;
    }
    if (tryHandleEngineCommand(cmd, args, jsonOutput, out, err, &exitCode)) {
      break;
    }

    PipeWireThread pw;
    PipeWireGraph graph(&pw);

    if (!pw.isConnected()) {
      err << "headroomctl: failed to connect to PipeWire\n";
      exitCode = 1;
      break;
    }

    waitForGraph(250);
    if (graph.nodes().isEmpty()) {
      err << "headroomctl: warning: no PipeWire nodes found (is PipeWire running? is XDG_RUNTIME_DIR set?)\n";
    }

    if (tryHandleGraphCommands(cmd, args, graph, jsonOutput, out, err, &exitCode)) {
      break;
    }
    if (tryHandlePatchbayCommand(cmd, args, graph, jsonOutput, out, err, &exitCode)) {
      break;
    }
    if (tryHandleSessionCommand(cmd, args, graph, jsonOutput, out, err, &exitCode)) {
      break;
    }
    if (tryHandleEqCommand(cmd, args, graph, jsonOutput, out, err, &exitCode)) {
      break;
    }
    if (tryHandleRecordPipeWire(app, cmd, args, pw, graph, jsonOutput, out, err, &exitCode)) {
      break;
    }

    printUsage(err);
    exitCode = 2;
  } while (false);

  return exitCode;
}
} // namespace headroomctl
