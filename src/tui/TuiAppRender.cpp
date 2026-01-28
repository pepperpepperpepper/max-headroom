#include "tui/TuiAppInternal.h"

#include "backend/EqManager.h"

#include <QSettings>

#include <algorithm>

#include <curses.h>

namespace headroomtui {

void renderTuiFrame(PipeWireGraph& graph, EqManager& eq, AudioRecorder& recorder, TuiState& state)
{
  int height = 0;
  int width = 0;
  getmaxyx(stdscr, height, width);

  erase();
  drawHeader(state.page, width);

  switch (state.page) {
  case Page::Outputs: {
    QSettings s;
    const QList<PwNodeInfo> sinks = applySinksOrder(graph.audioSinks(), s);
    drawListPage("Output Devices", sinks, &graph, state.selectedSink, graph.defaultAudioSinkId(), height, width);
    break;
  }
  case Page::Inputs: {
    const QList<PwNodeInfo> sources = graph.audioSources();
    drawListPage("Input Devices", sources, &graph, state.selectedSource, graph.defaultAudioSourceId(), height, width);
    break;
  }
  case Page::Streams:
    drawStreamsPage(&graph, state.selectedStream, height, width);
    break;
  case Page::Patchbay: {
    drawPatchbayPage(&graph, state.selectedLink, state.patchbayStatus, height, width);
    break;
  }
  case Page::Eq:
    drawEqPage(&graph, &eq, state.selectedEqDevice, state.eqStatus, height, width);
    break;
  case Page::Recording:
    drawRecordingPage(&graph,
                      &recorder,
                      state.recordingSnapshot ? &*state.recordingSnapshot : nullptr,
                      state.selectedRecordingTarget,
                      state.recordingPath,
                      state.recordingFormat,
                      state.recordingDurationLimitSec,
                      state.recordingStatus,
                      height,
                      width);
    break;
  case Page::Status:
    drawStatusPage(&graph, state.selectedStatus, height, width);
    break;
  case Page::Engine:
    drawEnginePage(state.engineUnits, state.selectedEngine, state.engineStatus, height, width);
    break;
  }

  QString statusLine;
  switch (state.page) {
  case Page::Outputs: {
    QSettings s;
    const QList<PwNodeInfo> sinks = applySinksOrder(graph.audioSinks(), s);
    const PwNodeInfo n = sinks.value(clampIndex(state.selectedSink, sinks.size()));
    const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
    const bool isDef = graph.defaultAudioSinkId().has_value() && graph.defaultAudioSinkId().value() == n.id;
    statusLine = nodeSummary(QStringLiteral("OUT"), n, c, isDef);
    break;
  }
  case Page::Inputs: {
    const QList<PwNodeInfo> sources = graph.audioSources();
    const PwNodeInfo n = sources.value(clampIndex(state.selectedSource, sources.size()));
    const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
    const bool isDef = graph.defaultAudioSourceId().has_value() && graph.defaultAudioSourceId().value() == n.id;
    statusLine = nodeSummary(QStringLiteral("IN"), n, c, isDef);
    break;
  }
  case Page::Streams: {
    QList<PwNodeInfo> streams = graph.audioPlaybackStreams();
    streams.append(graph.audioCaptureStreams());
    std::sort(streams.begin(), streams.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      if (a.mediaClass != b.mediaClass) {
        return a.mediaClass < b.mediaClass;
      }
      return a.description < b.description;
    });
    const PwNodeInfo n = streams.value(clampIndex(state.selectedStream, streams.size()));
    const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
    const bool isPlayback = n.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));
    const StreamRoute route = (n.id != 0u) ? routeForStream(&graph, n) : StreamRoute{};
    const QString routeText =
        route.deviceName.isEmpty()
            ? QStringLiteral("")
            : (route.isPlayback ? QStringLiteral(" -> %1").arg(route.deviceName) : QStringLiteral(" <- %1").arg(route.deviceName));
    statusLine = nodeSummary(isPlayback ? QStringLiteral("PB") : QStringLiteral("REC"), n, c, false) + routeText;
    break;
  }
  case Page::Patchbay: {
    QList<PwLinkInfo> links = graph.links();
    std::sort(links.begin(), links.end(), [](const PwLinkInfo& a, const PwLinkInfo& b) {
      if (a.outputNodeId != b.outputNodeId) {
        return a.outputNodeId < b.outputNodeId;
      }
      if (a.inputNodeId != b.inputNodeId) {
        return a.inputNodeId < b.inputNodeId;
      }
      if (a.outputPortId != b.outputPortId) {
        return a.outputPortId < b.outputPortId;
      }
      return a.inputPortId < b.inputPortId;
    });
    const PwLinkInfo l = links.value(clampIndex(state.selectedLink, links.size()));
    if (l.id != 0u) {
      statusLine = QStringLiteral("Link %1  out:%2/%3  in:%4/%5").arg(l.id).arg(l.outputNodeId).arg(l.outputPortId).arg(l.inputNodeId).arg(l.inputPortId);
    } else {
      statusLine = QStringLiteral("Patchbay: (no links)");
    }
    if (!state.patchbayStatus.trimmed().isEmpty()) {
      statusLine += QStringLiteral("  |  %1").arg(state.patchbayStatus);
    }
    break;
  }
  case Page::Eq: {
    QList<PwNodeInfo> targets = eqTargetsForGraph(&graph);
    const PwNodeInfo n = targets.value(clampIndex(state.selectedEqDevice, targets.size()));
    const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
    const EqPreset p = n.name.isEmpty() ? EqPreset{} : eq.presetForNodeName(n.name);
    statusLine = nodeSummary(QStringLiteral("EQ"), n, c, false);
    if (n.id != 0u) {
      statusLine += QStringLiteral("  |  %1").arg(p.enabled ? QStringLiteral("ON") : QStringLiteral("OFF"));
    }
    if (!state.eqStatus.trimmed().isEmpty()) {
      statusLine += QStringLiteral("  |  %1").arg(state.eqStatus);
    }
    break;
  }
  case Page::Recording: {
    const QString stateStr = recorder.isRecording() ? QStringLiteral("Recording") : QStringLiteral("Idle");
    statusLine = QStringLiteral("%1  |  %2").arg(QStringLiteral("REC")).arg(stateStr);
    if (!state.recordingStatus.trimmed().isEmpty()) {
      statusLine += QStringLiteral("  |  %1").arg(state.recordingStatus);
    }
    break;
  }
  case Page::Engine: {
    const SystemdUnitStatus st = state.engineUnits.value(clampIndex(state.selectedEngine, state.engineUnits.size()));
    if (!state.engineStatus.trimmed().isEmpty()) {
      statusLine = QStringLiteral("Engine: %1").arg(state.engineStatus);
    } else if (!st.unit.isEmpty()) {
      const QString active = st.activeState.isEmpty() ? QStringLiteral("-") : st.activeState;
      const QString sub = st.subState.isEmpty() ? QStringLiteral("-") : st.subState;
      statusLine = QStringLiteral("Engine %1  %2/%3").arg(st.unit, active, sub);
    } else {
      statusLine = QStringLiteral("Engine: (no units)");
    }
    if (!st.error.trimmed().isEmpty()) {
      statusLine += QStringLiteral("  |  %1").arg(st.error);
    }
    break;
  }
  case Page::Status: {
    const auto snapOpt = graph.profilerSnapshot();
    if (snapOpt.has_value() && snapOpt->seq > 0 && snapOpt->hasInfo) {
      statusLine = QStringLiteral("CPU %1/%2/%3  XRuns %4")
                       .arg(QString::number(snapOpt->cpuLoadFast * 100.0, 'f', 1) + "%")
                       .arg(QString::number(snapOpt->cpuLoadMedium * 100.0, 'f', 1) + "%")
                       .arg(QString::number(snapOpt->cpuLoadSlow * 100.0, 'f', 1) + "%")
                       .arg(snapOpt->xrunCount);
    } else {
      statusLine = QStringLiteral("Status: (no profiler data yet)");
    }
    break;
  }
  }

  if (!state.globalStatus.trimmed().isEmpty()) {
    statusLine += QStringLiteral("  |  %1").arg(state.globalStatus);
  }
  drawStatusBar(statusLine, height, width);

  if (state.showHelp) {
    drawHelpOverlay(state.page, height, width);
  }

  refresh();
}

} // namespace headroomtui
