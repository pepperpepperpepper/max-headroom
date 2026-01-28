#include "tui/TuiAppInternal.h"

#include "backend/EqManager.h"

#include <algorithm>

#include <curses.h>

namespace headroomtui {

namespace tui_actions_internal {
void handleDevicesKey(int ch, PipeWireGraph& graph, TuiState& state);
void handlePatchbayKey(int ch, PipeWireGraph& graph, TuiState& state);
} // namespace tui_actions_internal

void tickRecordingTimer(AudioRecorder& recorder, TuiState& state)
{
  if (recorder.isRecording() && state.recordingDurationLimitSec > 0) {
    const uint32_t sr = recorder.sampleRate();
    const uint64_t frames = recorder.framesCaptured();
    const double secs = (sr > 0) ? (static_cast<double>(frames) / static_cast<double>(sr)) : 0.0;
    if (secs >= static_cast<double>(state.recordingDurationLimitSec)) {
      const uint64_t bytes = recorder.dataBytesWritten();
      const QString path = recorder.filePath().isEmpty() ? state.recordingPath : recorder.filePath();
      recorder.stop();
      state.recordingStatus = QStringLiteral("Stopped (timer): %1 (%2 bytes)%3")
                                  .arg(path)
                                  .arg(static_cast<qulonglong>(bytes))
                                  .arg(bytes == 0 ? QStringLiteral("  (no audio received; is a session manager/driver running?)") : QString{});
      beep();
    }
  }
}

void refreshEngineStatusIfNeeded(TuiState& state)
{
  if (state.page == Page::Engine && (state.engineDirty || state.engineRefresh.elapsed() > 1000)) {
    state.engineUnits.clear();
    state.engineStatus.clear();

    const QStringList units = EngineControl::defaultUserUnits();
    QString err;
    if (!EngineControl::isSystemctlAvailable()) {
      state.engineStatus = QStringLiteral("systemctl not found (Engine controls unavailable).");
    } else if (!EngineControl::canTalkToUserSystemd(&err)) {
      state.engineStatus = QStringLiteral("systemd --user unavailable: %1").arg(err);
    }

    for (const auto& unit : units) {
      state.engineUnits.append(EngineControl::queryUserUnit(unit));
    }
    state.selectedEngine = clampIndex(state.selectedEngine, state.engineUnits.size());

    state.engineDirty = false;
    state.engineRefresh.restart();
  }
}

void handleTuiKey(int ch, PipeWireGraph& graph, EqManager& eq, AudioRecorder& recorder, TuiState& state)
{
  if (state.showHelp) {
    switch (ch) {
    case 27: // Esc
    case '\n':
    case KEY_ENTER:
    case 'q':
    case 'Q':
    case '?':
    case 'h':
    case 'H':
      state.showHelp = false;
      break;
    default:
      break;
    }
  } else if (ch == '?' || ch == 'h' || ch == 'H') {
    state.showHelp = true;
  } else {
    switch (ch) {
    case 'q':
    case 'Q':
      state.running = false;
      break;
    case '\t': {
      const int next = static_cast<int>(state.page) + 1;
      state.page = pageFromIndex(next);
      break;
    }
    case KEY_F(1):
    case '1':
      state.page = Page::Outputs;
      break;
    case KEY_F(2):
    case '2':
      state.page = Page::Inputs;
      break;
    case KEY_F(3):
    case '3':
      state.page = Page::Streams;
      break;
    case KEY_F(4):
    case '4':
      state.page = Page::Patchbay;
      break;
    case KEY_F(5):
    case '5':
      state.page = Page::Eq;
      break;
    case KEY_F(6):
    case '6':
      state.page = Page::Recording;
      break;
    case KEY_F(7):
    case '7':
      state.page = Page::Status;
      break;
    case KEY_F(8):
    case '8':
      state.page = Page::Engine;
      break;
    default:
      break;
    }

    if (state.page == Page::Outputs || state.page == Page::Inputs || state.page == Page::Streams) {
      tui_actions_internal::handleDevicesKey(ch, graph, state);
    } else if (state.page == Page::Patchbay) {
      tui_actions_internal::handlePatchbayKey(ch, graph, state);
    } else if (state.page == Page::Eq) {
      QList<PwNodeInfo> targets = eqTargetsForGraph(&graph);
      const int count = targets.size();

      state.selectedEqDevice = clampIndex(state.selectedEqDevice, count);

      const PwNodeInfo selectedNode = targets.isEmpty() ? PwNodeInfo{} : targets[state.selectedEqDevice];
      const QString nodeName = selectedNode.name;

      switch (ch) {
      case KEY_UP:
        state.selectedEqDevice = clampIndex(state.selectedEqDevice - 1, count);
        break;
      case KEY_DOWN:
        state.selectedEqDevice = clampIndex(state.selectedEqDevice + 1, count);
        break;
      case 'e':
      case 'E':
        if (!nodeName.isEmpty()) {
          EqPreset p = eq.presetForNodeName(nodeName);
          p.enabled = !p.enabled;
          eq.setPresetForNodeName(nodeName, p);
          state.eqStatus = QStringLiteral("%1: EQ %2").arg(displayNameForNode(selectedNode), p.enabled ? "enabled" : "disabled");
        } else {
          beep();
        }
        break;
      case 'p':
      case 'P':
      case '\n':
      case KEY_ENTER: {
        if (nodeName.isEmpty()) {
          beep();
          break;
        }

        int height = 0;
        int width = 0;
        getmaxyx(stdscr, height, width);

        const auto presets = builtinEqPresets();
        const int idx = promptSelectPresetIndex(presets, 0, height, width);
        if (idx < 0 || idx >= presets.size()) {
          break;
        }

        EqPreset next = presets[idx].second;
        next.enabled = true;
        eq.setPresetForNodeName(nodeName, next);
        state.eqStatus = QStringLiteral("%1: applied preset \"%2\"").arg(displayNameForNode(selectedNode), presets[idx].first);
        break;
      }
      default:
        break;
      }
    } else if (state.page == Page::Engine) {
      const int n = state.engineUnits.size();
      state.selectedEngine = clampIndex(state.selectedEngine, n);

      auto selectedUnit = [&]() -> QString {
        const SystemdUnitStatus st = state.engineUnits.value(state.selectedEngine);
        return st.unit;
      };

      auto doStart = [&]() {
        const QString unit = selectedUnit();
        if (unit.isEmpty()) {
          beep();
          state.globalStatus = QStringLiteral("No unit selected.");
          return;
        }

        QString err;
        const bool ok = EngineControl::startUserUnit(unit, &err);
        if (!ok) {
          state.globalStatus = err.isEmpty() ? QStringLiteral("Start failed.") : QStringLiteral("Start failed: %1").arg(err);
          beep();
          return;
        }
        state.globalStatus = QStringLiteral("Started: %1").arg(unit);
        state.engineDirty = true;
      };

      auto doStop = [&]() {
        const QString unit = selectedUnit();
        if (unit.isEmpty()) {
          beep();
          state.globalStatus = QStringLiteral("No unit selected.");
          return;
        }

        QString err;
        const bool ok = EngineControl::stopUserUnit(unit, &err);
        if (!ok) {
          state.globalStatus = err.isEmpty() ? QStringLiteral("Stop failed.") : QStringLiteral("Stop failed: %1").arg(err);
          beep();
          return;
        }
        state.globalStatus = QStringLiteral("Stopped: %1").arg(unit);
        state.engineDirty = true;
      };

      auto doRestart = [&]() {
        const QString unit = selectedUnit();
        if (unit.isEmpty()) {
          beep();
          state.globalStatus = QStringLiteral("No unit selected.");
          return;
        }

        QString err;
        const bool ok = EngineControl::restartUserUnit(unit, &err);
        if (!ok) {
          state.globalStatus = err.isEmpty() ? QStringLiteral("Restart failed.") : QStringLiteral("Restart failed: %1").arg(err);
          beep();
          return;
        }
        state.globalStatus = QStringLiteral("Restarted: %1").arg(unit);
        state.engineDirty = true;
      };

      switch (ch) {
      case KEY_UP:
        state.selectedEngine = clampIndex(state.selectedEngine - 1, n);
        break;
      case KEY_DOWN:
        state.selectedEngine = clampIndex(state.selectedEngine + 1, n);
        break;
      case 'g':
      case 'G':
        state.engineDirty = true;
        state.globalStatus = QStringLiteral("Engine: refresh requested.");
        break;
      case 's':
      case 'S':
        doStart();
        break;
      case 't':
      case 'T':
        doStop();
        break;
      case 'r':
      case 'R':
        doRestart();
        break;
      default:
        break;
      }
    } else if (state.page == Page::Recording) {
      const QVector<RecordingTarget> targets = buildRecordingTargets(&graph);
      state.selectedRecordingTarget = clampIndex(state.selectedRecordingTarget, targets.size());

      switch (ch) {
      case KEY_UP:
        state.selectedRecordingTarget = clampIndex(state.selectedRecordingTarget - 1, targets.size());
        break;
      case KEY_DOWN:
        state.selectedRecordingTarget = clampIndex(state.selectedRecordingTarget + 1, targets.size());
        break;
      case 'o':
      case 'O': {
        if (recorder.isRecording()) {
          beep();
          state.recordingStatus = QStringLiteral("Stop recording before changing format.");
          break;
        }
        state.recordingFormat = (state.recordingFormat == AudioRecorder::Format::Wav) ? AudioRecorder::Format::Flac : AudioRecorder::Format::Wav;
        state.recordingStatus = QStringLiteral("Format set: %1").arg(AudioRecorder::formatToString(state.recordingFormat));
        break;
      }
      case 't':
      case 'T': {
        if (recorder.isRecording()) {
          beep();
          state.recordingStatus = QStringLiteral("Stop recording before changing the timer.");
          break;
        }

        int height = 0;
        int width = 0;
        getmaxyx(stdscr, height, width);
        const QString cur = QString::number(std::max(0, state.recordingDurationLimitSec));
        const QString next = promptInputLine("Recording timer", "Stop after N seconds (0=off).", cur, height, width);
        if (next.isEmpty()) {
          break;
        }
        bool ok = false;
        const int v = next.toInt(&ok);
        if (!ok || v < 0) {
          beep();
          state.recordingStatus = QStringLiteral("Invalid timer value.");
          break;
        }
        state.recordingDurationLimitSec = v;
        state.recordingStatus = (state.recordingDurationLimitSec > 0) ? QStringLiteral("Timer set: %1 sec").arg(state.recordingDurationLimitSec)
                                                                      : QStringLiteral("Timer disabled.");
        break;
      }
      case 'f':
      case 'F': {
        if (recorder.isRecording()) {
          beep();
          state.recordingStatus = QStringLiteral("Stop recording before changing the file path.");
          break;
        }

        int height = 0;
        int width = 0;
        getmaxyx(stdscr, height, width);
        const QString next = promptInputLine(
            "Recording output",
            "Enter output path or template (.wav/.flac; supports {datetime} {target} {ext}).",
            state.recordingPath,
            height,
            width);
        if (!next.isEmpty()) {
          state.recordingPath = next;
          state.recordingStatus = QStringLiteral("File set: %1").arg(state.recordingPath);
        }
        break;
      }
      case 'r':
      case 'R':
      case '\n':
      case KEY_ENTER: {
        if (recorder.isRecording()) {
          const uint64_t bytes = recorder.dataBytesWritten();
          recorder.stop();
          state.recordingStatus = QStringLiteral("Stopped: %1 (%2 bytes)%3")
                                      .arg(recorder.filePath().isEmpty() ? state.recordingPath : recorder.filePath())
                                      .arg(static_cast<qulonglong>(bytes))
                                      .arg(bytes == 0 ? QStringLiteral("  (no audio received; is a session manager/driver running?)") : QString{});
        } else {
          const RecordingTarget t = targets.value(state.selectedRecordingTarget, RecordingTarget{});
          const QString resolvedPath = AudioRecorder::expandPathTemplate(state.recordingPath, t.label, state.recordingFormat);
          const RecordingGraphSnapshot snap = captureRecordingSnapshot(&graph);
          AudioRecorder::StartOptions o;
          o.filePath = resolvedPath;
          o.targetObject = t.targetObject;
          o.captureSink = t.captureSink;
          o.format = state.recordingFormat;
          const bool ok = recorder.start(o);
          if (!ok) {
            const QString err = recorder.lastError();
            state.recordingStatus = err.isEmpty() ? QStringLiteral("Start failed.") : QStringLiteral("Start failed: %1").arg(err);
            beep();
          } else {
            state.recordingStatus = QStringLiteral("Recording started.");
            state.recordingSnapshot = snap;
          }
        }
        break;
      }
      case 'm':
      case 'M': {
        int height = 0;
        int width = 0;
        getmaxyx(stdscr, height, width);
        if (!state.recordingSnapshot) {
          beep();
          state.recordingStatus = QStringLiteral("No metadata snapshot yet (start a recording first).");
          break;
        }
        promptScrollLines("Recording metadata", recordingSnapshotLines(*state.recordingSnapshot), height, width);
        break;
      }
      default:
        break;
      }
    } else if (state.page == Page::Status) {
      const auto snapOpt = graph.profilerSnapshot();
      const int n = snapOpt.has_value() ? snapOpt->drivers.size() : 0;
      switch (ch) {
      case KEY_UP:
        state.selectedStatus = clampIndex(state.selectedStatus - 1, n);
        break;
      case KEY_DOWN:
        state.selectedStatus = clampIndex(state.selectedStatus + 1, n);
        break;
      default:
        break;
      }
    }
  }
}

} // namespace headroomtui
