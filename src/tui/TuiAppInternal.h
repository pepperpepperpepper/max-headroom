#pragma once

#include "tui/TuiInternal.h"

#include <QElapsedTimer>

#include <cstdint>
#include <optional>

namespace headroomtui {

struct TuiState final {
  Page page = Page::Outputs;
  int selectedSink = 0;
  int selectedSource = 0;
  int selectedStream = 0;
  int selectedLink = 0;
  int selectedEqDevice = 0;
  int selectedRecordingTarget = 0;
  int selectedStatus = 0;
  int selectedEngine = 0;

  uint32_t patchbayOutNodeId = 0;
  uint32_t patchbayOutPortId = 0;
  uint32_t patchbayInNodeId = 0;
  uint32_t patchbayInPortId = 0;
  QString patchbayStatus;
  QString eqStatus;
  QString recordingStatus;
  std::optional<RecordingGraphSnapshot> recordingSnapshot;
  AudioRecorder::Format recordingFormat = AudioRecorder::Format::Wav;
  int recordingDurationLimitSec = 0;
  QString recordingPath = QStringLiteral("headroom-recording-{datetime}-{target}.{ext}");

  bool running = true;
  bool showHelp = false;
  QString globalStatus;

  QList<SystemdUnitStatus> engineUnits;
  QString engineStatus;
  QElapsedTimer engineRefresh;
  bool engineDirty = true;
};

void tickRecordingTimer(AudioRecorder& recorder, TuiState& state);
void refreshEngineStatusIfNeeded(TuiState& state);
void handleTuiKey(int ch, PipeWireGraph& graph, EqManager& eq, AudioRecorder& recorder, TuiState& state);
void renderTuiFrame(PipeWireGraph& graph, EqManager& eq, AudioRecorder& recorder, TuiState& state);

} // namespace headroomtui
