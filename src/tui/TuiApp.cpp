#include "tui/TuiAppInternal.h"

#include "backend/EqManager.h"
#include "backend/PipeWireThread.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

#include <curses.h>

namespace headroomtui {

void printUsage()
{
  std::printf(
      "headroom-tui %s\n"
      "\n"
      "Usage:\n"
      "  headroom-tui\n"
      "  headroom-tui --help\n"
      "  headroom-tui --version\n"
      "\n"
      "Keys:\n"
      "  Tab/F1-F8 pages  Up/Down select  Left/Right or +/- volume  m mute  ? help\n"
      "  [ / ]                Reorder outputs (Outputs)\n"
      "  S/T/R                Engine start/stop/restart (Engine)\n"
      "  Enter default/move/connect  c connect  d disconnect  e EQ toggle  p EQ preset\n"
      "  r rec  f file  q quit\n",
      HEADROOM_VERSION);
}

int runTui(QCoreApplication& app, QStringList args)
{
  int exitCode = 0;
  if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
    printUsage();
    exitCode = 0;
  } else if (args.contains(QStringLiteral("--version")) || args.contains(QStringLiteral("-V"))) {
    std::printf("%s\n", HEADROOM_VERSION);
    exitCode = 0;
  } else {
    PipeWireThread pw;
    PipeWireGraph graph(&pw);
    EqManager eq(&pw, &graph);
    AudioRecorder recorder(&pw);

    if (!pw.isConnected()) {
      std::fprintf(stderr, "headroom-tui: failed to connect to PipeWire\n");
      exitCode = 1;
    } else {
      initscr();
      cbreak();
      noecho();
      keypad(stdscr, TRUE);
      curs_set(0);
      timeout(kMainLoopTimeoutMs);

      TuiState state;
      QObject::connect(&recorder, &AudioRecorder::errorOccurred, &app, [&](const QString& msg) {
        state.recordingStatus = QStringLiteral("Error: %1").arg(msg);
        beep();
      });

      state.engineRefresh.start();

      while (state.running) {
        QCoreApplication::processEvents();

        tickRecordingTimer(recorder, state);
        refreshEngineStatusIfNeeded(state);

        const int ch = getch();
        if (ch != ERR) {
          handleTuiKey(ch, graph, eq, recorder, state);
        }

        renderTuiFrame(graph, eq, recorder, state);
      }

      endwin();
    }
  }

  return exitCode;
}

} // namespace headroomtui
