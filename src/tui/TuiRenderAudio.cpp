#include "tui/TuiInternal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <curses.h>

namespace headroomtui {

static const char* pageName(Page p)
{
  switch (p) {
  case Page::Outputs:
    return "Outputs";
  case Page::Inputs:
    return "Inputs";
  case Page::Streams:
    return "Streams";
  case Page::Patchbay:
    return "Patchbay";
  case Page::Eq:
    return "EQ";
  case Page::Recording:
    return "Recording";
  case Page::Status:
    return "Status";
  case Page::Engine:
    return "Engine";
  }
  return "Unknown";
}

static void drawBar(int y, int x, int width, float volume)
{
  width = std::max(0, width);
  const float normalized = clampVolume(volume) / kMaxVolume;
  const int fill = std::clamp(static_cast<int>(std::round(normalized * width)), 0, width);

  mvaddch(y, x, '[');
  for (int i = 0; i < width; i++) {
    mvaddch(y, x + 1 + i, (i < fill) ? '#' : ' ');
  }
  mvaddch(y, x + 1 + width, ']');
}

void drawHeader(Page page, int width)
{
  const char* title = "headroom-tui";
  const char* pageStr = pageName(page);

  mvprintw(0, 0, "%s  |  %s", title, pageStr);
  if (width > 0) {
    for (int x = std::min(width - 1, static_cast<int>(std::strlen(title) + std::strlen(pageStr) + 8)); x < width; x++) {
      mvaddch(0, x, ' ');
    }
  }

  mvprintw(1, 0, "Tab/F1-F8 pages  Up/Down select  Left/Right or +/- volume  m mute  ? help  q quit");
}

void drawStatusBar(const QString& text, int height, int width)
{
  if (height <= 0 || width <= 0) {
    return;
  }

  const int y = height - 1;
  attron(A_REVERSE);
  mvhline(y, 0, ' ', width);
  const QByteArray utf8 = text.toUtf8();
  mvaddnstr(y, 0, utf8.constData(), std::max(0, width - 1));
  attroff(A_REVERSE);
}

void drawHelpOverlay(Page page, int height, int width)
{
  if (height <= 0 || width <= 0) {
    return;
  }

  QStringList lines;
  lines << QStringLiteral("Close: Esc / q / ? / h");
  lines << QString();
  lines << QStringLiteral("Global:");
  lines << QStringLiteral("  Tab / F1-F8 / 1-8    Switch pages");
  lines << QStringLiteral("  ? or h               Toggle this help");
  lines << QStringLiteral("  q                    Quit");
  lines << QString();
  lines << QStringLiteral("Navigation:");
  lines << QStringLiteral("  Up/Down (j/k)        Select item");
  lines << QStringLiteral("  Left/Right or -/+    Volume (Outputs/Inputs/Streams)");
  lines << QStringLiteral("  [ / ]                Reorder outputs (Outputs)");
  lines << QStringLiteral("  m                    Mute/unmute");
  lines << QString();
  lines << QStringLiteral("Actions:");
  lines << QStringLiteral("  Enter                Default device / Move / Connect / Preset / Rec (page-dependent)");
  lines << QStringLiteral("  c                    Connect ports (Patchbay)");
  lines << QStringLiteral("  d                    Delete selected link (Patchbay)");
  lines << QStringLiteral("  e                    Toggle EQ (EQ)");
  lines << QStringLiteral("  p                    Apply EQ preset (EQ)");
  lines << QStringLiteral("  r                    Start/stop recording (Recording)");
  lines << QStringLiteral("  f                    Recording file/template");
  lines << QStringLiteral("  o                    Recording format (wav/flac)");
  lines << QStringLiteral("  t                    Recording timer");
  lines << QStringLiteral("  m                    Recording metadata (Recording)");
  lines << QStringLiteral("  S/T/R                Start/stop/restart (Engine)");

  int maxLen = 0;
  for (const auto& l : lines) {
    maxLen = std::max(maxLen, static_cast<int>(l.size()));
  }
  const QString title = QStringLiteral("Help — %1").arg(QString::fromUtf8(pageName(page)));
  maxLen = std::max(maxLen, static_cast<int>(title.size()));

  const int boxW = std::min(width - 4, maxLen + 4);
  const int boxH = std::min(height - 2, static_cast<int>(lines.size()) + 4);
  if (boxW < 10 || boxH < 6) {
    return;
  }

  const int y0 = std::max(0, (height - boxH) / 2);
  const int x0 = std::max(0, (width - boxW) / 2);

  for (int y = 0; y < boxH; ++y) {
    mvhline(y0 + y, x0, ' ', boxW);
  }

  mvaddch(y0, x0, ACS_ULCORNER);
  mvhline(y0, x0 + 1, ACS_HLINE, boxW - 2);
  mvaddch(y0, x0 + boxW - 1, ACS_URCORNER);

  for (int y = 1; y < boxH - 1; ++y) {
    mvaddch(y0 + y, x0, ACS_VLINE);
    mvaddch(y0 + y, x0 + boxW - 1, ACS_VLINE);
  }

  mvaddch(y0 + boxH - 1, x0, ACS_LLCORNER);
  mvhline(y0 + boxH - 1, x0 + 1, ACS_HLINE, boxW - 2);
  mvaddch(y0 + boxH - 1, x0 + boxW - 1, ACS_LRCORNER);

  attron(A_BOLD);
  const QByteArray titleUtf8 = title.toUtf8();
  mvaddnstr(y0 + 1, x0 + 2, titleUtf8.constData(), std::max(0, boxW - 4));
  attroff(A_BOLD);

  const int innerW = std::max(0, boxW - 4);
  const int maxLines = std::max(0, boxH - 4);
  for (int i = 0; i < lines.size() && i < maxLines; ++i) {
    const QByteArray utf8 = lines.at(i).toUtf8();
    mvaddnstr(y0 + 2 + i, x0 + 2, utf8.constData(), innerW);
  }
}

void drawListPage(const char* title,
                  const QList<PwNodeInfo>& devices,
                  PipeWireGraph* graph,
                  int& selectedIdx,
                  std::optional<uint32_t> defaultNodeId,
                  int height,
                  int width)
{
  const int deviceCount = static_cast<int>(devices.size());
  mvprintw(3, 0, "%s (%d)", title, deviceCount);

  const int listTop = 5;
  const int listHeight = std::max(0, height - listTop - 2);

  if (deviceCount <= 0) {
    mvprintw(listTop, 0, "(none)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, deviceCount);

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, deviceCount - listHeight));

  for (int row = 0; row < listHeight; row++) {
    const int idx = start + row;
    if (idx >= deviceCount) {
      break;
    }

    const auto& node = devices[idx];
    const QString name = displayNameForNode(node);
    const auto controlsOpt = graph->nodeControls(node.id);
    const PwNodeControls controls = controlsOpt.value_or(PwNodeControls{});

    const int volPct = static_cast<int>(std::round(controls.volume * 100.0f));
    const char* muteStr = controls.mute ? "MUTED" : "     ";
    const bool isDefault = defaultNodeId.has_value() && node.id == *defaultNodeId;

    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const int y = listTop + row;
    mvprintw(y, 0, "%c %4u%c  %-5s  %4d%%  ", selected ? '>' : ' ', node.id, isDefault ? '*' : ' ', muteStr, volPct);

    const int nameX = 0 + 2 + 4 + 1 + 2 + 5 + 2 + 5 + 2;
    const int barWidth = std::max(10, std::min(24, width - nameX - 40));
    const int barX = width - (barWidth + 2) - 1;
    const int nameWidth = std::max(0, barX - nameX - 2);

    const QByteArray nameUtf8 = name.toUtf8();
    mvaddnstr(y, nameX, nameUtf8.constData(), nameWidth);

    drawBar(y, barX, barWidth, controls.volume);

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

void drawStreamsPage(PipeWireGraph* graph, int& selectedIdx, int height, int width)
{
  if (!graph) {
    mvprintw(3, 0, "Streams");
    mvprintw(6, 0, "(no graph)");
    return;
  }

  QList<PwNodeInfo> streams = graph->audioPlaybackStreams();
  streams.append(graph->audioCaptureStreams());

  std::sort(streams.begin(), streams.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    if (a.mediaClass != b.mediaClass) {
      return a.mediaClass < b.mediaClass;
    }
    return a.description < b.description;
  });

  const int streamCount = static_cast<int>(streams.size());
  mvprintw(3, 0, "Streams (%d)", streamCount);

  const int listTop = 5;
  const int listHeight = std::max(0, height - listTop - 2);

  if (streamCount <= 0) {
    mvprintw(listTop, 0, "(none)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, streamCount);

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, streamCount - listHeight));

  for (int row = 0; row < listHeight; row++) {
    const int idx = start + row;
    if (idx >= streamCount) {
      break;
    }

    const auto& node = streams[idx];
    const bool isPlayback = node.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));

    const QString name = displayNameForNode(node);
    const auto controlsOpt = graph->nodeControls(node.id);
    const PwNodeControls controls = controlsOpt.value_or(PwNodeControls{});

    const StreamRoute route = routeForStream(graph, node);

    const char* typeStr = isPlayback ? "PB" : "REC";
    const int volPct = static_cast<int>(std::round(controls.volume * 100.0f));
    const char* muteStr = controls.mute ? "MUTED" : "     ";

    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const int y = listTop + row;
    mvprintw(y, 0, "%c %4u  %-3s  %-5s  %4d%%  ", selected ? '>' : ' ', node.id, typeStr, muteStr, volPct);

    const QString routeText = route.deviceName.isEmpty()
        ? QStringLiteral("")
        : (route.isPlayback ? QStringLiteral("-> %1").arg(route.deviceName) : QStringLiteral("<- %1").arg(route.deviceName));
    const QByteArray routeUtf8 = routeText.toUtf8();
    const int routeX = 0 + 1 + 5 + 2 + 3 + 2 + 5 + 2 + 5 + 2;
    mvaddnstr(y, routeX, routeUtf8.constData(), 22);

    const int nameX = routeX + 24;
    const int barWidth = std::max(10, std::min(24, width - nameX - 40));
    const int barX = width - (barWidth + 2) - 1;
    const int nameWidth = std::max(0, barX - nameX - 2);

    const QByteArray nameUtf8 = name.toUtf8();
    mvaddnstr(y, nameX, nameUtf8.constData(), nameWidth);

    drawBar(y, barX, barWidth, controls.volume);

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

} // namespace headroomtui

