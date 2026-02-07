#include "tui/TuiAppInternal.h"

#include "backend/EqManager.h"
#include "backend/PipeWireThread.h"

#include <QCoreApplication>
#include <QObject>
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
      "  headroom-tui --self-test-idle\n"
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
  } else if (args.contains(QStringLiteral("--self-test-idle"))) {
    PipeWireThread pw;
    PipeWireGraph graph(&pw);
    EqManager eq(&pw, &graph);
    AudioRecorder recorder(&pw);

    if (!pw.isConnected()) {
      std::fprintf(stderr, "headroom-tui: self-test: failed to connect to PipeWire\n");
      exitCode = 1;
    } else {
      initscr();
      cbreak();
      noecho();
      keypad(stdscr, TRUE);
      curs_set(0);
      timeout(kMainLoopTimeoutMs);

      TuiState state;
      QObject stateCtx;

      QObject::connect(&recorder, &AudioRecorder::errorOccurred, &app, [&](const QString& msg) {
        state.recordingStatus = QStringLiteral("Error: %1").arg(msg);
        state.dirty = true;
      });

      QObject::connect(&graph, &PipeWireGraph::graphChanged, &stateCtx, [&]() { state.dirty = true; });

      state.engineRefresh.start();
      state.lastRender.start();
      state.clock.start();

      constexpr int kWarmupMs = 1200;
      constexpr int kMeasureMs = 2000;
      constexpr int kMaxRenders = 20;

      QElapsedTimer testTimer;
      QElapsedTimer measureTimer;
      testTimer.start();
      bool counting = false;
      int renderCount = 0;

      while (state.running) {
        QCoreApplication::processEvents();

        flushPendingVolumeChanges(graph, state);
        tickRecordingTimer(recorder, state);
        refreshEngineStatusIfNeeded(state);

        const int ch = getch();
        if (ch != ERR) {
          // Keep normal behavior so we still exercise input responsiveness.
          handleTuiKey(ch, graph, eq, recorder, state);
          state.dirty = true;
        }

        const int refreshEveryMs =
            (state.page == Page::Recording && recorder.isRecording()) ? 100 : ((state.page == Page::Status) ? 250 : 750);
        if (!state.dirty && state.lastRender.isValid() && state.lastRender.elapsed() > refreshEveryMs) {
          state.dirty = true;
        }

        if (state.dirty) {
          renderTuiFrame(graph, eq, recorder, state);
          state.dirty = false;
          state.lastRender.restart();
          if (counting) {
            ++renderCount;
          }
        }

        if (!counting && testTimer.isValid() && testTimer.elapsed() >= kWarmupMs) {
          counting = true;
          renderCount = 0;
          measureTimer.start();
        }
        if (counting && measureTimer.isValid() && measureTimer.elapsed() >= kMeasureMs) {
          state.running = false;
        }
      }

      flushPendingVolumeChanges(graph, state, true);
      endwin();

      if (!counting) {
        std::fprintf(stderr, "headroom-tui: self-test: did not reach measurement window\n");
        exitCode = 1;
      } else if (renderCount > kMaxRenders) {
        std::fprintf(stderr, "headroom-tui: self-test: too many redraws while idle: %d (max %d)\n", renderCount, kMaxRenders);
        exitCode = 1;
      } else {
        exitCode = 0;
      }
    }
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
      QObject stateCtx;
      QObject::connect(&recorder, &AudioRecorder::errorOccurred, &app, [&](const QString& msg) {
        state.recordingStatus = QStringLiteral("Error: %1").arg(msg);
        state.dirty = true;
        beep();
      });

      QObject::connect(&graph, &PipeWireGraph::graphChanged, &stateCtx, [&]() { state.dirty = true; });

      state.engineRefresh.start();
      state.lastRender.start();
      state.clock.start();

      while (state.running) {
        QCoreApplication::processEvents();

        flushPendingVolumeChanges(graph, state);
        tickRecordingTimer(recorder, state);
        refreshEngineStatusIfNeeded(state);

        const int ch = getch();
        if (ch != ERR) {
          handleTuiKey(ch, graph, eq, recorder, state);
          state.dirty = true;
        }

        const int refreshEveryMs =
            (state.page == Page::Recording && recorder.isRecording()) ? 100 : ((state.page == Page::Status) ? 250 : 750);
        if (!state.dirty && state.lastRender.isValid() && state.lastRender.elapsed() > refreshEveryMs) {
          state.dirty = true;
        }

        if (state.dirty) {
          renderTuiFrame(graph, eq, recorder, state);
          state.dirty = false;
          state.lastRender.restart();
        }
      }

      flushPendingVolumeChanges(graph, state, true);
      endwin();
    }
  }

  return exitCode;
}

} // namespace headroomtui
