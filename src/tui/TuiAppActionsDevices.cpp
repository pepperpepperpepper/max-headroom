#include "tui/TuiAppInternal.h"

#include <QCoreApplication>

#include <algorithm>

#include <curses.h>

namespace headroomtui {
namespace tui_actions_internal {

void handleDevicesKey(int ch, PipeWireGraph& graph, TuiState& state)
{
  QList<PwNodeInfo> devices;
  int* selPtr = nullptr;

  if (state.page == Page::Outputs) {
    QSettings s;
    devices = applySinksOrder(graph.audioSinks(), s);
    selPtr = &state.selectedSink;
  } else if (state.page == Page::Inputs) {
    devices = graph.audioSources();
    selPtr = &state.selectedSource;
  } else {
    devices = graph.audioPlaybackStreams();
    devices.append(graph.audioCaptureStreams());
    std::sort(devices.begin(), devices.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      if (a.mediaClass != b.mediaClass) {
        return a.mediaClass < b.mediaClass;
      }
      return a.description < b.description;
    });
    selPtr = &state.selectedStream;
  }

  int& sel = *selPtr;

  sel = clampIndex(sel, devices.size());
  const PwNodeInfo selectedNode = devices.isEmpty() ? PwNodeInfo{} : devices[sel];
  const uint32_t nodeId = devices.isEmpty() ? 0u : selectedNode.id;
  const PwNodeControls ctrls = graph.nodeControls(nodeId).value_or(PwNodeControls{});

  auto trySetDefault = [&](bool sink) {
    if (nodeId == 0u) {
      beep();
      return;
    }
    if (!graph.hasDefaultDeviceSupport()) {
      state.globalStatus = QStringLiteral("Default device controls unavailable.");
      beep();
      return;
    }

    bool ok = false;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 1200) {
      ok = sink ? graph.setDefaultAudioSink(nodeId) : graph.setDefaultAudioSource(nodeId);
      if (ok) {
        break;
      }
      QCoreApplication::processEvents();
      napms(40);
    }

    if (!ok) {
      state.globalStatus = sink ? QStringLiteral("Failed to set default output.") : QStringLiteral("Failed to set default input.");
      beep();
      return;
    }

    state.globalStatus = sink ? QStringLiteral("Default output: %1").arg(displayNameForNode(selectedNode))
                              : QStringLiteral("Default input: %1").arg(displayNameForNode(selectedNode));
  };

  auto doMoveStream = [&]() {
    if (state.page != Page::Streams || nodeId == 0u) {
      beep();
      return;
    }

    int height = 0;
    int width = 0;
    getmaxyx(stdscr, height, width);

    const StreamRoute route = routeForStream(&graph, selectedNode);
    const bool isPlayback = selectedNode.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));

    if (isPlayback) {
      const QList<PwNodeInfo> sinks = graph.audioSinks();
      const uint32_t sinkId = promptSelectNodeId("Move playback stream to output device", sinks, route.deviceId, height, width);
      if (sinkId != 0 && sinkId != route.deviceId) {
        const bool ok = movePlaybackStreamToSink(&graph, nodeId, sinkId);
        if (!ok) {
          beep();
        }
      }
    } else {
      const QList<PwNodeInfo> sources = graph.audioSources();
      const uint32_t sourceId = promptSelectNodeId("Move capture stream to input device", sources, route.deviceId, height, width);
      if (sourceId != 0 && sourceId != route.deviceId) {
        const bool ok = moveCaptureStreamToSource(&graph, nodeId, sourceId);
        if (!ok) {
          beep();
        }
      }
    }
  };

  switch (ch) {
  case KEY_UP:
    sel = clampIndex(sel - 1, devices.size());
    break;
  case KEY_DOWN:
    sel = clampIndex(sel + 1, devices.size());
    break;
  case '[':
  case ']':
    if (state.page == Page::Outputs && !selectedNode.name.isEmpty()) {
      const int delta = (ch == '[') ? -1 : 1;
      const QString selectedName = selectedNode.name;
      const QList<PwNodeInfo> sinksNow = graph.audioSinks();
      QString statusMsg;
      const bool ok = moveSinkInOrder(sinksNow, selectedName, delta, &statusMsg);
      if (!statusMsg.isEmpty()) {
        state.globalStatus = statusMsg;
      }
      if (ok) {
        QSettings s;
        devices = applySinksOrder(sinksNow, s);
        int idx = 0;
        for (int i = 0; i < devices.size(); ++i) {
          if (devices[i].name == selectedName) {
            idx = i;
            break;
          }
        }
        sel = clampIndex(idx, devices.size());
      } else {
        beep();
      }
    }
    break;
  case KEY_LEFT:
  case '-':
    if (nodeId != 0u) {
      graph.setNodeVolume(nodeId, clampVolume(ctrls.volume - kVolumeStep));
    }
    break;
  case KEY_RIGHT:
  case '+':
  case '=':
    if (nodeId != 0u) {
      graph.setNodeVolume(nodeId, clampVolume(ctrls.volume + kVolumeStep));
    }
    break;
  case 'm':
  case 'M':
    if (nodeId != 0u) {
      graph.setNodeMute(nodeId, !ctrls.mute);
    }
    break;
  case '\n':
  case KEY_ENTER:
    if (state.page == Page::Outputs) {
      trySetDefault(true);
    } else if (state.page == Page::Inputs) {
      trySetDefault(false);
    } else if (state.page == Page::Streams) {
      doMoveStream();
    } else {
      beep();
    }
    break;
  case 't':
  case 'T':
    if (state.page == Page::Streams) {
      doMoveStream();
    }
    break;
  default:
    break;
  }
}

} // namespace tui_actions_internal
} // namespace headroomtui

