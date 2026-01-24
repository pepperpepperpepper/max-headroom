#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMultiHash>
#include <QPair>
#include <QSet>
#include <QSettings>
#include <QStringList>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <optional>
#include <locale.h>

#include <curses.h>

#include "backend/AudioRecorder.h"
#include "backend/EngineControl.h"
#include "backend/PatchbayPortConfig.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "backend/EqManager.h"
#include "settings/SettingsKeys.h"

namespace {

constexpr float kMinVolume = 0.0f;
constexpr float kMaxVolume = 2.0f;
constexpr float kVolumeStep = 0.05f;
constexpr int kMainLoopTimeoutMs = 80;

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

enum class Page : int {
  Outputs = 0,
  Inputs = 1,
  Streams = 2,
  Patchbay = 3,
  Eq = 4,
  Recording = 5,
  Status = 6,
  Engine = 7,
};

const char* pageName(Page p)
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

Page pageFromIndex(int idx)
{
  idx = (idx % 8 + 8) % 8;
  return static_cast<Page>(idx);
}

int clampIndex(int idx, int size)
{
  if (size <= 0) {
    return 0;
  }
  return std::clamp(idx, 0, size - 1);
}

float clampVolume(float v)
{
  return std::clamp(v, kMinVolume, kMaxVolume);
}

QString displayNameForNode(const PwNodeInfo& n)
{
  if (!n.description.isEmpty()) {
    return n.description;
  }
  if (!n.name.isEmpty()) {
    return n.name;
  }
  return QStringLiteral("(unnamed)");
}

QStringList defaultSinksOrder(const QList<PwNodeInfo>& sinks)
{
  QList<PwNodeInfo> sorted = sinks;
  std::sort(sorted.begin(), sorted.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
  });

  QStringList order;
  order.reserve(sorted.size());
  for (const auto& n : sorted) {
    if (!n.name.isEmpty()) {
      order.push_back(n.name);
    }
  }
  return order;
}

QList<PwNodeInfo> applySinksOrder(const QList<PwNodeInfo>& sinks, QSettings& s)
{
  const QStringList saved = s.value(SettingsKeys::sinksOrder()).toStringList();
  if (saved.isEmpty()) {
    QList<PwNodeInfo> sorted = sinks;
    std::sort(sorted.begin(), sorted.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
    });
    return sorted;
  }

  QHash<QString, PwNodeInfo> byName;
  byName.reserve(sinks.size());
  for (const auto& node : sinks) {
    byName.insert(node.name, node);
  }

  QStringList used;
  QList<PwNodeInfo> ordered;
  used.reserve(saved.size());
  ordered.reserve(sinks.size());

  for (const auto& name : saved) {
    if (!byName.contains(name)) {
      continue;
    }
    ordered.push_back(byName.value(name));
    used.push_back(name);
  }

  QList<PwNodeInfo> remaining;
  remaining.reserve(sinks.size());
  for (const auto& node : sinks) {
    if (!used.contains(node.name)) {
      remaining.push_back(node);
    }
  }
  std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
  });
  ordered.append(remaining);
  return ordered;
}

bool moveSinkInOrder(const QList<PwNodeInfo>& sinks, const QString& sinkName, int delta, QString* statusOut = nullptr)
{
  if (sinkName.trimmed().isEmpty()) {
    if (statusOut) {
      *statusOut = QStringLiteral("No sink selected.");
    }
    return false;
  }

  QSettings s;
  QStringList order = s.value(SettingsKeys::sinksOrder()).toStringList();

  if (order.isEmpty()) {
    order = defaultSinksOrder(sinks);
  } else {
    const QSet<QString> known = QSet<QString>(order.begin(), order.end());
    QList<PwNodeInfo> remaining;
    remaining.reserve(sinks.size());
    for (const auto& n : sinks) {
      if (!known.contains(n.name)) {
        remaining.push_back(n);
      }
    }
    std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
    });
    for (const auto& n : remaining) {
      order.push_back(n.name);
    }
  }

  int idx = order.indexOf(sinkName);
  if (idx < 0) {
    order.push_back(sinkName);
    idx = order.size() - 1;
  }

  const int next = idx + delta;
  if (next < 0 || next >= order.size()) {
    if (statusOut) {
      *statusOut = QStringLiteral("Already at %1.").arg(delta < 0 ? QStringLiteral("top") : QStringLiteral("bottom"));
    }
    return false;
  }

  order.swapItemsAt(idx, next);

  const QStringList def = defaultSinksOrder(sinks);
  const bool storeCustom = !def.isEmpty() && order != def;
  if (storeCustom) {
    s.setValue(SettingsKeys::sinksOrder(), order);
  } else {
    s.remove(SettingsKeys::sinksOrder());
  }

  if (statusOut) {
    *statusOut = QStringLiteral("Output order updated.");
  }
  return true;
}

QString displayNameForPort(const PwPortInfo& p)
{
  if (!p.name.isEmpty()) {
    return p.name;
  }
  if (!p.alias.isEmpty()) {
    return p.alias;
  }
  return QStringLiteral("(unnamed)");
}

QString displayNameForPort(const PwPortInfo& p, const QString& nodeName, QSettings& settings)
{
  if (const auto custom = PatchbayPortConfigStore::customAlias(settings, nodeName, p.name)) {
    return *custom;
  }
  return displayNameForPort(p);
}

enum class PortKind {
  Audio,
  Midi,
  Other,
};

PortKind portKindFor(const PwPortInfo& p, const QHash<uint32_t, PwNodeInfo>& nodesById)
{
  const QString mt = p.mediaType.trimmed().toLower();
  if (mt == QStringLiteral("midi")) {
    return PortKind::Midi;
  }
  if (mt == QStringLiteral("audio")) {
    return PortKind::Audio;
  }
  if (!p.audioChannel.isEmpty()) {
    return PortKind::Audio;
  }
  if (p.formatDsp.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (p.formatDsp.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  const auto it = nodesById.constFind(p.nodeId);
  if (it != nodesById.constEnd()) {
    if (it->mediaClass.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
      return PortKind::Midi;
    }
    if (it->mediaClass.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
      return PortKind::Audio;
    }
  }
  if (p.name.contains(QStringLiteral("midi"), Qt::CaseInsensitive) || p.alias.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  return PortKind::Other;
}

const char* portKindLabelShort(PortKind k)
{
  switch (k) {
    case PortKind::Audio:
      return "AUD";
    case PortKind::Midi:
      return "MIDI";
    case PortKind::Other:
      return "OTH";
  }
  return "OTH";
}

void drawBar(int y, int x, int width, float volume)
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

const char* eqBandTypeShort(EqBandType t)
{
  switch (t) {
  case EqBandType::Peaking:
    return "PK";
  case EqBandType::LowShelf:
    return "LS";
  case EqBandType::HighShelf:
    return "HS";
  case EqBandType::LowPass:
    return "LP";
  case EqBandType::HighPass:
    return "HP";
  case EqBandType::Notch:
    return "NT";
  case EqBandType::BandPass:
    return "BP";
  }
  return "PK";
}

QVector<QPair<QString, EqPreset>> builtinEqPresets()
{
  QVector<QPair<QString, EqPreset>> out;

  EqPreset flat = defaultEqPreset(6);
  flat.enabled = true;

  EqPreset bass = flat;
  if (!bass.bands.isEmpty()) {
    bass.bands[0].type = EqBandType::LowShelf;
    bass.bands[0].freqHz = 90.0;
    bass.bands[0].q = 0.707;
    bass.bands[0].gainDb = 4.0;
  }
  if (bass.bands.size() > 1) {
    bass.bands[1].gainDb = 2.0;
  }

  EqPreset treble = flat;
  if (treble.bands.size() > 4) {
    treble.bands[4].gainDb = 2.5;
  }
  if (treble.bands.size() > 5) {
    treble.bands[5].type = EqBandType::HighShelf;
    treble.bands[5].freqHz = 8000.0;
    treble.bands[5].q = 0.707;
    treble.bands[5].gainDb = 4.0;
  }

  EqPreset vocal = flat;
  if (vocal.bands.size() > 2) {
    vocal.bands[2].gainDb = -1.0;
  }
  if (vocal.bands.size() > 3) {
    vocal.bands[3].gainDb = 3.0;
    vocal.bands[3].q = 1.2;
  }

  EqPreset loudness = flat;
  if (!loudness.bands.isEmpty()) {
    loudness.bands[0].type = EqBandType::LowShelf;
    loudness.bands[0].freqHz = 90.0;
    loudness.bands[0].q = 0.707;
    loudness.bands[0].gainDb = 3.5;
  }
  if (loudness.bands.size() > 3) {
    loudness.bands[3].gainDb = -2.0;
  }
  if (loudness.bands.size() > 5) {
    loudness.bands[5].type = EqBandType::HighShelf;
    loudness.bands[5].freqHz = 9000.0;
    loudness.bands[5].q = 0.707;
    loudness.bands[5].gainDb = 3.0;
  }

  out.push_back(qMakePair(QStringLiteral("Flat"), flat));
  out.push_back(qMakePair(QStringLiteral("Bass Boost"), bass));
  out.push_back(qMakePair(QStringLiteral("Treble Boost"), treble));
  out.push_back(qMakePair(QStringLiteral("Vocal"), vocal));
  out.push_back(qMakePair(QStringLiteral("Loudness (V)"), loudness));
  return out;
}

int promptSelectPresetIndex(const QVector<QPair<QString, EqPreset>>& presets, int currentIndex, int height, int width)
{
  const int presetCount = static_cast<int>(presets.size());
  if (presetCount <= 0 || height <= 0 || width <= 0) {
    return -1;
  }

  int selected = clampIndex(currentIndex, presetCount);

  timeout(-1);
  while (true) {
    erase();
    mvprintw(0, 0, "Select EQ preset");
    mvprintw(1, 0, "Up/Down select  Enter confirm  Esc cancel");

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 2);

    selected = clampIndex(selected, presetCount);

    int start = 0;
    if (selected >= listHeight) {
      start = selected - listHeight + 1;
    }
    start = std::clamp(start, 0, std::max(0, presetCount - listHeight));

    for (int row = 0; row < listHeight; ++row) {
      const int idx = start + row;
      if (idx >= presetCount) {
        break;
      }
      const bool isSel = (idx == selected);
      if (isSel) {
        attron(A_REVERSE);
      }

      const QByteArray nameUtf8 = presets[idx].first.toUtf8();
      mvprintw(listTop + row, 0, "%c ", isSel ? '>' : ' ');
      mvaddnstr(listTop + row, 2, nameUtf8.constData(), std::max(0, width - 3));

      if (isSel) {
        attroff(A_REVERSE);
      }
    }

    refresh();

    const int ch = getch();
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
      timeout(kMainLoopTimeoutMs);
      return -1;
    case KEY_UP:
    case 'k':
    case 'K':
      selected = clampIndex(selected - 1, presetCount);
      break;
    case KEY_DOWN:
    case 'j':
    case 'J':
      selected = clampIndex(selected + 1, presetCount);
      break;
    case '\n':
    case KEY_ENTER:
      timeout(kMainLoopTimeoutMs);
      return selected;
    default:
      break;
    }
  }
}

QString summarizeEqPreset(const EqPreset& preset)
{
  QStringList parts;
  parts << QStringLiteral("pre %1dB").arg(preset.preampDb, 0, 'f', 1);

  QStringList gains;
  gains.reserve(preset.bands.size());
  for (const auto& b : preset.bands) {
    if (!b.enabled) {
      gains << QStringLiteral("--");
      continue;
    }
    gains << QStringLiteral("%1").arg(b.gainDb, 0, 'f', 1);
  }
  if (!gains.isEmpty()) {
    parts << QStringLiteral("gains[%1]").arg(gains.join(','));
  }
  return parts.join(QStringLiteral("  "));
}

void drawEqPage(PipeWireGraph* graph, EqManager* eq, int& selectedIdx, const QString& statusLine, int height, int width)
{
  mvprintw(3, 0, "EQ");

  if (!graph || !eq) {
    mvprintw(6, 0, "(no graph)");
    return;
  }

  QList<PwNodeInfo> targets = graph->audioSinks();
  const int sinkCount = targets.size();
  const QList<PwNodeInfo> sources = graph->audioSources();
  targets.append(sources);

  mvprintw(4,
           0,
           "Devices: %d  (Outputs: %d, Inputs: %d)",
           static_cast<int>(targets.size()),
           sinkCount,
           static_cast<int>(sources.size()));
  mvprintw(5, 0, "Up/Down select  e toggle  p/Enter preset");

  if (!statusLine.isEmpty()) {
    const QByteArray statusUtf8 = statusLine.toUtf8();
    mvaddnstr(6, 0, statusUtf8.constData(), std::max(0, width - 1));
  }

  const int count = static_cast<int>(targets.size());
  const int listTop = 8;
  const int listHeight = std::max(0, height - listTop - 4);

  if (count <= 0) {
    mvprintw(listTop, 0, "(no devices)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, count);

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, count - listHeight));

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= count) {
      break;
    }

    const auto& node = targets[idx];
    const EqPreset preset = eq->presetForNodeName(node.name);

    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const char* kind = node.mediaClass == QStringLiteral("Audio/Sink") ? "OUT" : "IN ";
    const char* enabled = preset.enabled ? "ON " : "OFF";

    const QString name = displayNameForNode(node);
    const QByteArray nameUtf8 = name.toUtf8();

    mvprintw(listTop + row, 0, "%c %4u  %s  %s  pre %+5.1f  ", selected ? '>' : ' ', node.id, kind, enabled, preset.preampDb);
    mvaddnstr(listTop + row, 32, nameUtf8.constData(), std::max(0, width - 34));

    if (selected) {
      attroff(A_REVERSE);
    }
  }

  const PwNodeInfo selectedNode = targets.value(selectedIdx);
  const EqPreset preset = eq->presetForNodeName(selectedNode.name);

  QString detail = QStringLiteral("Selected: %1  |  EQ %2  |  %3")
                       .arg(displayNameForNode(selectedNode))
                       .arg(preset.enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
                       .arg(summarizeEqPreset(preset));
  const QByteArray detailUtf8 = detail.toUtf8();
  mvaddnstr(std::max(0, height - 3), 0, detailUtf8.constData(), std::max(0, width - 1));

  int bandY = std::max(0, height - 2);
  if (bandY > listTop + 1 && preset.bands.size() > 0) {
    QString bandLine = QStringLiteral("Bands:");
    for (int i = 0; i < preset.bands.size(); ++i) {
      const auto& b = preset.bands[i];
      bandLine += QStringLiteral(" %1:%2@%3Hz%4")
                      .arg(eqBandTypeShort(b.type))
                      .arg(b.gainDb, 0, 'f', 1)
                      .arg(b.freqHz, 0, 'f', 0)
                      .arg(b.enabled ? "" : " (off)");
    }
    const QByteArray bandUtf8 = bandLine.toUtf8();
    mvaddnstr(bandY, 0, bandUtf8.constData(), std::max(0, width - 1));
  }
}

struct RecordingTarget final {
  QString label;
  QString targetObject;
  bool captureSink = true;
};

struct RecordingGraphSnapshot final {
  QString capturedAtUtc;
  QList<PwNodeInfo> sinks;
  QList<PwNodeInfo> sources;
  QList<PwNodeInfo> playbackStreams;
  QList<PwNodeInfo> captureStreams;
  std::optional<uint32_t> defaultSinkId;
  std::optional<uint32_t> defaultSourceId;
};

RecordingGraphSnapshot captureRecordingSnapshot(PipeWireGraph* graph)
{
  RecordingGraphSnapshot snap;
  snap.capturedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  if (!graph) {
    return snap;
  }

  snap.sinks = graph->audioSinks();
  snap.sources = graph->audioSources();
  snap.playbackStreams = graph->audioPlaybackStreams();
  snap.captureStreams = graph->audioCaptureStreams();
  snap.defaultSinkId = graph->defaultAudioSinkId();
  snap.defaultSourceId = graph->defaultAudioSourceId();
  return snap;
}

QString formatSnapshotNodeLine(const PwNodeInfo& n)
{
  const QString base = displayNameForNode(n);
  const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
  if (!app.isEmpty() && app != base) {
    return QStringLiteral("%1  %2 — %3  (%4)").arg(n.id, 4).arg(app, base, n.mediaClass);
  }
  return QStringLiteral("%1  %2  (%3)").arg(n.id, 4).arg(base, n.mediaClass);
}

QStringList recordingSnapshotLines(const RecordingGraphSnapshot& snap)
{
  QStringList lines;
  lines.push_back(QStringLiteral("Captured at (UTC): %1").arg(snap.capturedAtUtc));
  lines.push_back(QStringLiteral("Default sink id: %1").arg(snap.defaultSinkId ? QString::number(*snap.defaultSinkId) : QStringLiteral("(unknown)")));
  lines.push_back(QStringLiteral("Default source id: %1").arg(snap.defaultSourceId ? QString::number(*snap.defaultSourceId) : QStringLiteral("(unknown)")));
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Output devices (%1):").arg(snap.sinks.size()));
  for (const auto& n : snap.sinks) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Input devices (%1):").arg(snap.sources.size()));
  for (const auto& n : snap.sources) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Playback streams (%1):").arg(snap.playbackStreams.size()));
  for (const auto& n : snap.playbackStreams) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Capture streams (%1):").arg(snap.captureStreams.size()));
  for (const auto& n : snap.captureStreams) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }

  return lines;
}

QVector<RecordingTarget> buildRecordingTargets(PipeWireGraph* graph)
{
  QVector<RecordingTarget> targets;
  targets.push_back({QStringLiteral("System mix (default output monitor)"), QString{}, true});

  if (graph) {
    const QList<PwNodeInfo> sinks = graph->audioSinks();
    for (const auto& s : sinks) {
      targets.push_back({QStringLiteral("Output: %1").arg(displayNameForNode(s)), s.name, true});
    }

    const QList<PwNodeInfo> playback = graph->audioPlaybackStreams();
    for (const auto& n : playback) {
      const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
      const QString name = displayNameForNode(n);
      const QString label = (!app.isEmpty() && app != name) ? QStringLiteral("%1 — %2").arg(app, name) : name;
      targets.push_back({QStringLiteral("App playback: %1").arg(label), n.name, false});
    }

    targets.push_back({QStringLiteral("Default input (mic)"), QString{}, false});
    const QList<PwNodeInfo> sources = graph->audioSources();
    for (const auto& s : sources) {
      targets.push_back({QStringLiteral("Input: %1").arg(displayNameForNode(s)), s.name, false});
    }

    const QList<PwNodeInfo> capture = graph->audioCaptureStreams();
    for (const auto& n : capture) {
      const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
      const QString name = displayNameForNode(n);
      const QString label = (!app.isEmpty() && app != name) ? QStringLiteral("%1 — %2").arg(app, name) : name;
      targets.push_back({QStringLiteral("App recording: %1").arg(label), n.name, true});
    }
  }

  return targets;
}

void promptScrollLines(const char* title, const QStringList& lines, int height, int width)
{
  timeout(-1);
  noecho();
  curs_set(0);

  int top = 0;
  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvaddnstr(1, 0, "Up/Down scroll  PgUp/PgDn page  q to close", std::max(0, width - 1));

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 1);
    const int totalLines = static_cast<int>(lines.size());
    const int maxTop = std::max(0, totalLines - listHeight);
    top = std::clamp(top, 0, maxTop);

    for (int row = 0; row < listHeight; ++row) {
      const int idx = top + row;
      if (idx >= totalLines) {
        break;
      }
      const QByteArray utf8 = lines.at(idx).toUtf8();
      mvaddnstr(listTop + row, 0, utf8.constData(), std::max(0, width - 1));
    }

    refresh();
    const int ch = getch();
    if (ch == 'q' || ch == 'Q' || ch == 27) {
      break;
    }
    if (ch == KEY_UP) {
      top = std::max(0, top - 1);
    } else if (ch == KEY_DOWN) {
      top = std::min(maxTop, top + 1);
    } else if (ch == KEY_PPAGE) {
      top = std::max(0, top - listHeight);
    } else if (ch == KEY_NPAGE) {
      top = std::min(maxTop, top + listHeight);
    }
  }

  timeout(kMainLoopTimeoutMs);
}

QString promptInputLine(const char* title, const char* prompt, const QString& currentValue, int height, int width)
{
  timeout(-1);
  echo();
  curs_set(1);

  erase();
  mvprintw(0, 0, "%s", title);
  mvaddnstr(2, 0, prompt, std::max(0, width - 1));

  if (!currentValue.isEmpty()) {
    const QByteArray curUtf8 = QStringLiteral("Current: %1").arg(currentValue).toUtf8();
    mvaddnstr(3, 0, curUtf8.constData(), std::max(0, width - 1));
  }

  mvaddnstr(std::max(0, height - 2), 0, "Enter to accept, blank to cancel.", std::max(0, width - 1));
  mvprintw(5, 0, "> ");

  char buf[1024];
  std::memset(buf, 0, sizeof(buf));
  getnstr(buf, static_cast<int>(sizeof(buf) - 1));

  noecho();
  curs_set(0);
  timeout(kMainLoopTimeoutMs);

  return QString::fromLocal8Bit(buf).trimmed();
}

QString formatDuration(double seconds)
{
  if (!std::isfinite(seconds) || seconds < 0.0) {
    seconds = 0.0;
  }
  const int total = static_cast<int>(seconds + 0.5);
  const int mm = total / 60;
  const int ss = total % 60;
  return QStringLiteral("%1:%2").arg(mm, 2, 10, QLatin1Char('0')).arg(ss, 2, 10, QLatin1Char('0'));
}

QString describeRecorderTarget(const AudioRecorder* recorder, const QVector<RecordingTarget>& targets)
{
  if (!recorder) {
    return QStringLiteral("(no recorder)");
  }

  const QString obj = recorder->targetObject();
  const bool sink = recorder->captureSink();
  for (const auto& t : targets) {
    if (t.captureSink == sink && t.targetObject == obj) {
      return t.label;
    }
  }

  if (obj.isEmpty()) {
    return sink ? QStringLiteral("System mix (default output monitor)") : QStringLiteral("Default input (mic)");
  }

  return QStringLiteral("%1: %2").arg(sink ? QStringLiteral("Target sink") : QStringLiteral("Target source"), obj);
}

void drawRecordingPage(PipeWireGraph* graph,
                       AudioRecorder* recorder,
                       const RecordingGraphSnapshot* snapshot,
                       int& selectedTargetIdx,
                       const QString& filePathOrTemplate,
                       AudioRecorder::Format selectedFormat,
                       int durationLimitSec,
                       const QString& statusLine,
                       int height,
                       int width)
{
  mvprintw(3, 0, "Recording");
  mvprintw(4, 0, "Up/Down select target  r/Enter start/stop  f path/template  o format  t timer  m metadata");

  const QVector<RecordingTarget> targets = buildRecordingTargets(graph);
  selectedTargetIdx = clampIndex(selectedTargetIdx, targets.size());

  const bool isRec = recorder && recorder->isRecording();
  const uint32_t rate = recorder ? recorder->sampleRate() : 0u;
  const uint32_t ch = recorder ? recorder->channels() : 0u;
  const uint32_t quantum = recorder ? recorder->quantumFrames() : 0u;
  const uint64_t bytes = recorder ? recorder->dataBytesWritten() : 0u;
  const uint64_t frames = recorder ? recorder->framesCaptured() : 0u;

  const double seconds = (rate > 0) ? (static_cast<double>(frames) / static_cast<double>(rate)) : 0.0;

  const QString stateStr = isRec ? QStringLiteral("Recording (%1)").arg(recorder->streamStateString()) : QStringLiteral("Idle");
  const QByteArray stateUtf8 = QStringLiteral("State: %1").arg(stateStr).toUtf8();
  mvaddnstr(6, 0, stateUtf8.constData(), std::max(0, width - 1));

  int line = 7;
  if (isRec) {
    const QByteArray fileUtf8 =
        QStringLiteral("File: %1").arg(recorder->filePath().isEmpty() ? QStringLiteral("(unset)") : recorder->filePath()).toUtf8();
    mvaddnstr(line, 0, fileUtf8.constData(), std::max(0, width - 1));
    ++line;
  } else {
    const QByteArray fileUtf8 =
        QStringLiteral("Path/template: %1").arg(filePathOrTemplate.isEmpty() ? QStringLiteral("(unset)") : filePathOrTemplate).toUtf8();
    mvaddnstr(line, 0, fileUtf8.constData(), std::max(0, width - 1));
    ++line;

    if (!filePathOrTemplate.isEmpty() && filePathOrTemplate.contains(QLatin1Char('{')) && !targets.isEmpty()) {
      const QString preview = AudioRecorder::expandPathTemplate(filePathOrTemplate, targets[selectedTargetIdx].label, selectedFormat);
      const QByteArray previewUtf8 = QStringLiteral("Preview: %1").arg(preview).toUtf8();
      mvaddnstr(line, 0, previewUtf8.constData(), std::max(0, width - 1));
      ++line;
    }
  }

  const QString targetStr = isRec ? describeRecorderTarget(recorder, targets)
                                  : (targets.isEmpty() ? QStringLiteral("(no targets)") : targets[selectedTargetIdx].label);
  const QByteArray targetUtf8 = QStringLiteral("Target: %1").arg(targetStr).toUtf8();
  mvaddnstr(line, 0, targetUtf8.constData(), std::max(0, width - 1));
  ++line;

  const AudioRecorder::Format fmt = isRec ? recorder->format() : selectedFormat;
  const QString fmtStr = (fmt == AudioRecorder::Format::Wav) ? QStringLiteral("WAV f32") : QStringLiteral("FLAC pcm24");

  const QByteArray fmtUtf8 =
      QStringLiteral("Format: %1  |  %2 Hz  |  %3 ch  |  %4  |  %5 bytes")
          .arg(fmtStr)
          .arg(rate == 0 ? 48000 : static_cast<int>(rate))
          .arg(ch == 0 ? 2 : static_cast<int>(ch))
          .arg(formatDuration(seconds))
          .arg(static_cast<qulonglong>(bytes))
          .toUtf8();
  mvaddnstr(line, 0, fmtUtf8.constData(), std::max(0, width - 1));
  ++line;

  if (snapshot) {
    const QString qStr = quantum > 0 ? QString::number(quantum) : QStringLiteral("-");
    const QByteArray metaUtf8 =
        QStringLiteral("Metadata: quantum %1 frames  |  snapshot outputs %2  inputs %3  streams %4/%5  (press m)")
            .arg(qStr)
            .arg(snapshot->sinks.size())
            .arg(snapshot->sources.size())
            .arg(snapshot->playbackStreams.size())
            .arg(snapshot->captureStreams.size())
            .toUtf8();
    mvaddnstr(line, 0, metaUtf8.constData(), std::max(0, width - 1));
    ++line;
  }

  const float peakDb = recorder ? recorder->peakDb() : -100.0f;
  const float rmsDb = recorder ? recorder->rmsDb() : -100.0f;
  const QString peakStr = isRec ? QString::number(peakDb, 'f', 1) : QStringLiteral("-");
  const QString rmsStr = isRec ? QString::number(rmsDb, 'f', 1) : QStringLiteral("-");
  const QString timerStr = durationLimitSec > 0 ? QStringLiteral("stop after %1").arg(formatDuration(durationLimitSec)) : QStringLiteral("off");

  const QByteArray lvlUtf8 = QStringLiteral("Levels: peak %1 dBFS  rms %2 dBFS  |  Timer: %3")
                                 .arg(peakStr)
                                 .arg(rmsStr)
                                 .arg(timerStr)
                                 .toUtf8();
  mvaddnstr(line, 0, lvlUtf8.constData(), std::max(0, width - 1));
  ++line;

  if (!statusLine.isEmpty()) {
    const QByteArray statusUtf8 = statusLine.toUtf8();
    mvaddnstr(line, 0, statusUtf8.constData(), std::max(0, width - 1));
    ++line;
  }

  const int count = targets.size();
  const int listTop = line + 1;
  const int listHeight = std::max(0, height - listTop - 3);

  if (count <= 0) {
    mvprintw(listTop, 0, "(no targets)");
    return;
  }

  int start = 0;
  if (selectedTargetIdx >= listHeight) {
    start = selectedTargetIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, count - listHeight));

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= count) {
      break;
    }

    const bool selected = (idx == selectedTargetIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QByteArray labelUtf8 = targets[idx].label.toUtf8();
    mvprintw(listTop + row, 0, "%c ", selected ? '>' : ' ');
    mvaddnstr(listTop + row, 2, labelUtf8.constData(), std::max(0, width - 3));

    if (selected) {
      attroff(A_REVERSE);
    }
  }

  mvaddnstr(std::max(0, height - 2), 0, "Tip: templates help avoid overwriting (e.g. {datetime}-{target}.{ext}).", std::max(0, width - 1));
}

struct StreamRoute final {
  uint32_t deviceId = 0;
  QString deviceName;
  bool isPlayback = true;
};

StreamRoute routeForStream(PipeWireGraph* graph, const PwNodeInfo& stream)
{
  if (!graph) {
    return {};
  }

  const bool isPlayback = stream.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));

  const QList<PwNodeInfo> nodes = graph->nodes();
  const QList<PwLinkInfo> links = graph->links();

  QHash<uint32_t, PwNodeInfo> nodesById;
  nodesById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodesById.insert(n.id, n);
  }

  QMultiHash<uint32_t, uint32_t> forward;
  QMultiHash<uint32_t, uint32_t> backward;
  forward.reserve(links.size());
  backward.reserve(links.size());
  for (const auto& l : links) {
    if (l.outputNodeId != 0 && l.inputNodeId != 0) {
      forward.insert(l.outputNodeId, l.inputNodeId);
      backward.insert(l.inputNodeId, l.outputNodeId);
    }
  }

  const QString wanted = isPlayback ? QStringLiteral("Audio/Sink") : QStringLiteral("Audio/Source");

  struct QueueItem {
    uint32_t nodeId = 0;
    int depth = 0;
  };

  constexpr int kMaxDepth = 6;

  QList<QueueItem> queue;
  queue.reserve(64);
  QSet<uint32_t> visited;
  visited.reserve(64);

  visited.insert(stream.id);
  queue.push_back({stream.id, 0});

  while (!queue.isEmpty()) {
    const QueueItem item = queue.takeFirst();
    if (item.depth > kMaxDepth) {
      continue;
    }

    if (item.nodeId != stream.id) {
      const auto it = nodesById.constFind(item.nodeId);
      if (it != nodesById.constEnd() && it->mediaClass == wanted) {
        return StreamRoute{it->id, displayNameForNode(*it), isPlayback};
      }
    }

    if (item.depth == kMaxDepth) {
      continue;
    }

    const auto nexts = isPlayback ? forward.values(item.nodeId) : backward.values(item.nodeId);
    for (uint32_t next : nexts) {
      if (visited.contains(next)) {
        continue;
      }
      visited.insert(next);
      queue.push_back({next, item.depth + 1});
    }
  }

  return StreamRoute{0, {}, isPlayback};
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

QList<PwPortInfo> portsForNode(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction)
{
  QList<PwPortInfo> out;
  for (const auto& p : ports) {
    if (p.nodeId == nodeId && p.direction == direction) {
      out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end(), [](const PwPortInfo& a, const PwPortInfo& b) { return a.id < b.id; });
  return out;
}

QList<PwNodeInfo> nodesWithPortDirection(const QList<PwNodeInfo>& nodes, const QList<PwPortInfo>& ports, const QString& direction)
{
  QSet<uint32_t> allowed;
  allowed.reserve(ports.size());
  for (const auto& p : ports) {
    if (p.direction == direction && p.nodeId != 0) {
      allowed.insert(p.nodeId);
    }
  }

  QList<PwNodeInfo> out;
  out.reserve(nodes.size());
  for (const auto& n : nodes) {
    if (allowed.contains(n.id)) {
      out.push_back(n);
    }
  }

  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    if (a.mediaClass != b.mediaClass) {
      return a.mediaClass < b.mediaClass;
    }
    if (a.description != b.description) {
      return a.description < b.description;
    }
    return a.name < b.name;
  });

  return out;
}

bool movePlaybackStreamToSink(PipeWireGraph* graph, uint32_t streamId, uint32_t sinkId)
{
  if (!graph || streamId == 0 || sinkId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamOutPorts = portsForNode(ports, streamId, QStringLiteral("out"));
  const QList<PwPortInfo> sinkInPorts = portsForNode(ports, sinkId, QStringLiteral("in"));
  if (streamOutPorts.isEmpty() || sinkInPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamOutPortIds;
  streamOutPortIds.reserve(streamOutPorts.size());
  for (const auto& p : streamOutPorts) {
    streamOutPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.outputNodeId == streamId && streamOutPortIds.contains(l.outputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sinkByChannel;
  sinkByChannel.reserve(sinkInPorts.size());
  for (const auto& p : sinkInPorts) {
    if (!p.audioChannel.isEmpty()) {
      sinkByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamOutPorts.size(); ++i) {
    const auto& sp = streamOutPorts[i];
    uint32_t targetPortId = 0;

    if (!sp.audioChannel.isEmpty() && sinkByChannel.contains(sp.audioChannel)) {
      targetPortId = sinkByChannel.value(sp.audioChannel);
    } else if (i < sinkInPorts.size()) {
      targetPortId = sinkInPorts[i].id;
    }

    if (targetPortId != 0) {
      anyOk = graph->createLink(streamId, sp.id, sinkId, targetPortId) || anyOk;
    }
  }

  return anyOk;
}

bool moveCaptureStreamToSource(PipeWireGraph* graph, uint32_t streamId, uint32_t sourceId)
{
  if (!graph || streamId == 0 || sourceId == 0) {
    return false;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QList<PwLinkInfo> links = graph->links();

  const QList<PwPortInfo> streamInPorts = portsForNode(ports, streamId, QStringLiteral("in"));
  const QList<PwPortInfo> sourceOutPorts = portsForNode(ports, sourceId, QStringLiteral("out"));
  if (streamInPorts.isEmpty() || sourceOutPorts.isEmpty()) {
    return false;
  }

  QSet<uint32_t> streamInPortIds;
  streamInPortIds.reserve(streamInPorts.size());
  for (const auto& p : streamInPorts) {
    streamInPortIds.insert(p.id);
  }

  for (const auto& l : links) {
    if (l.inputNodeId == streamId && streamInPortIds.contains(l.inputPortId)) {
      graph->destroyLink(l.id);
    }
  }

  QHash<QString, uint32_t> sourceByChannel;
  sourceByChannel.reserve(sourceOutPorts.size());
  for (const auto& p : sourceOutPorts) {
    if (!p.audioChannel.isEmpty()) {
      sourceByChannel.insert(p.audioChannel, p.id);
    }
  }

  bool anyOk = false;
  for (int i = 0; i < streamInPorts.size(); ++i) {
    const auto& sp = streamInPorts[i];
    uint32_t sourcePortId = 0;

    if (!sp.audioChannel.isEmpty() && sourceByChannel.contains(sp.audioChannel)) {
      sourcePortId = sourceByChannel.value(sp.audioChannel);
    } else if (i < sourceOutPorts.size()) {
      sourcePortId = sourceOutPorts[i].id;
    }

    if (sourcePortId != 0) {
      anyOk = graph->createLink(sourceId, sourcePortId, streamId, sp.id) || anyOk;
    }
  }

  return anyOk;
}

uint32_t promptSelectPortId(const char* title,
                            const QList<PwPortInfo>& ports,
                            uint32_t currentId,
                            int height,
                            int width,
                            const QHash<uint32_t, PwNodeInfo>& nodesById)
{
  const int portCount = static_cast<int>(ports.size());
  if (!title || portCount <= 0 || height <= 0 || width <= 0) {
    return 0;
  }

  QSettings portSettings;

  int selected = 0;
  if (currentId != 0) {
    for (int i = 0; i < portCount; ++i) {
      if (ports[i].id == currentId) {
        selected = i;
        break;
      }
    }
  }

  timeout(-1);

  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "Up/Down select  Enter confirm  Esc cancel");

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 2);

    selected = clampIndex(selected, portCount);

    int start = 0;
    if (selected >= listHeight) {
      start = selected - listHeight + 1;
    }
    start = std::clamp(start, 0, std::max(0, portCount - listHeight));

    for (int row = 0; row < listHeight; ++row) {
      const int idx = start + row;
      if (idx >= portCount) {
        break;
      }
      const bool isSel = (idx == selected);
      if (isSel) {
        attron(A_REVERSE);
      }

      const auto& p = ports[idx];
      const QString nodeName = nodesById.value(p.nodeId, PwNodeInfo{}).name;
      const QString name = displayNameForPort(p, nodeName, portSettings);

      const QByteArray chanUtf8 = p.audioChannel.toUtf8();
      const QByteArray nameUtf8 = name.toUtf8();

      mvprintw(listTop + row, 0, "%c %4u  ", isSel ? '>' : ' ', p.id);
      if (!p.audioChannel.isEmpty()) {
        mvprintw(listTop + row, 8, "%-6s  ", chanUtf8.constData());
      } else {
        const PortKind kind = portKindFor(p, nodesById);
        mvprintw(listTop + row, 8, "%-6s  ", portKindLabelShort(kind));
      }
      mvaddnstr(listTop + row, 17, nameUtf8.constData(), std::max(0, width - 19));

      if (isSel) {
        attroff(A_REVERSE);
      }
    }

    refresh();

    const int ch = getch();
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
      timeout(kMainLoopTimeoutMs);
      return 0;
    case KEY_UP:
    case 'k':
    case 'K':
      selected = clampIndex(selected - 1, portCount);
      break;
    case KEY_DOWN:
    case 'j':
    case 'J':
      selected = clampIndex(selected + 1, portCount);
      break;
    case '\n':
    case KEY_ENTER:
      timeout(kMainLoopTimeoutMs);
      return ports[selected].id;
    default:
      break;
    }
  }
}

uint32_t promptSelectNodeId(const char* title, const QList<PwNodeInfo>& nodes, uint32_t currentId, int height, int width)
{
  const int nodeCount = static_cast<int>(nodes.size());
  if (!title || nodeCount <= 0 || height <= 0 || width <= 0) {
    return 0;
  }

  int selected = 0;
  if (currentId != 0) {
    for (int i = 0; i < nodeCount; ++i) {
      if (nodes[i].id == currentId) {
        selected = i;
        break;
      }
    }
  }

  timeout(-1);

  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "Up/Down select  Enter confirm  Esc cancel");

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 2);

    selected = clampIndex(selected, nodeCount);

    int start = 0;
    if (selected >= listHeight) {
      start = selected - listHeight + 1;
    }
    start = std::clamp(start, 0, std::max(0, nodeCount - listHeight));

    for (int row = 0; row < listHeight; ++row) {
      const int idx = start + row;
      if (idx >= nodeCount) {
        break;
      }
      const bool isSel = (idx == selected);
      if (isSel) {
        attron(A_REVERSE);
      }

      const auto& n = nodes[idx];
      const QString name = displayNameForNode(n);
      const QByteArray nameUtf8 = name.toUtf8();
      mvprintw(listTop + row, 0, "%c %4u  ", isSel ? '>' : ' ', n.id);
      mvaddnstr(listTop + row, 8, nameUtf8.constData(), std::max(0, width - 10));

      if (isSel) {
        attroff(A_REVERSE);
      }
    }

    refresh();

    const int ch = getch();
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
      timeout(kMainLoopTimeoutMs);
      return 0;
    case KEY_UP:
    case 'k':
    case 'K':
      selected = clampIndex(selected - 1, nodeCount);
      break;
    case KEY_DOWN:
    case 'j':
    case 'J':
      selected = clampIndex(selected + 1, nodeCount);
      break;
    case '\n':
    case KEY_ENTER:
      timeout(kMainLoopTimeoutMs);
      return nodes[selected].id;
    default:
      break;
    }
  }
}

void drawPatchbayPage(PipeWireGraph* graph, int& selectedLinkIdx, const QString& statusLine, int height, int width)
{
  mvprintw(3, 0, "Patchbay");

  if (!graph) {
    mvprintw(6, 0, "(no graph)");
    return;
  }

  const QList<PwNodeInfo> nodes = graph->nodes();
  const QList<PwPortInfo> ports = graph->ports();
  QList<PwLinkInfo> links = graph->links();

  QSettings portSettings;

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

  mvprintw(4,
           0,
           "Nodes: %d  Ports: %d  Links: %d",
           static_cast<int>(nodes.size()),
           static_cast<int>(ports.size()),
           static_cast<int>(links.size()));
  mvprintw(5, 0, "c/Enter connect  d delete link  Up/Down select");

  if (!statusLine.isEmpty()) {
    const QByteArray statusUtf8 = statusLine.toUtf8();
    mvaddnstr(6, 0, statusUtf8.constData(), std::max(0, width - 1));
  }

  const int linkCount = static_cast<int>(links.size());
  const int listTop = 8;
  const int listHeight = std::max(0, height - listTop - 2);

  if (linkCount <= 0) {
    mvprintw(listTop, 0, "(no links)");
    return;
  }

  QHash<uint32_t, QString> nodeNames;
  nodeNames.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodeNames.insert(n.id, displayNameForNode(n));
  }

  QHash<uint32_t, PwNodeInfo> nodesById;
  nodesById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodesById.insert(n.id, n);
  }

  QHash<uint32_t, QString> portNames;
  portNames.reserve(ports.size());
  for (const auto& p : ports) {
    const QString nodeName = nodesById.value(p.nodeId, PwNodeInfo{}).name;
    portNames.insert(p.id, displayNameForPort(p, nodeName, portSettings));
  }

  selectedLinkIdx = clampIndex(selectedLinkIdx, linkCount);

  int start = 0;
  if (selectedLinkIdx >= listHeight) {
    start = selectedLinkIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, linkCount - listHeight));

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= linkCount) {
      break;
    }

    const auto& l = links[idx];
    const bool selected = (idx == selectedLinkIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString outNode = nodeNames.value(l.outputNodeId, QStringLiteral("node %1").arg(l.outputNodeId));
    const QString outPort = portNames.value(l.outputPortId, QStringLiteral("port %1").arg(l.outputPortId));
    const QString inNode = nodeNames.value(l.inputNodeId, QStringLiteral("node %1").arg(l.inputNodeId));
    const QString inPort = portNames.value(l.inputPortId, QStringLiteral("port %1").arg(l.inputPortId));
    const QString text = QStringLiteral("%1:%2 -> %3:%4").arg(outNode, outPort, inNode, inPort);
    const QByteArray textUtf8 = text.toUtf8();

    const int y = listTop + row;
    mvprintw(y, 0, "%c %4u  ", selected ? '>' : ' ', l.id);
    mvaddnstr(y, 8, textUtf8.constData(), std::max(0, width - 10));

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

void drawStatusPage(PipeWireGraph* graph, int& selectedIdx, int height, int width)
{
  mvprintw(3, 0, "Status / diagnostics");

  if (!graph || !graph->hasProfilerSupport()) {
    mvprintw(5, 0, "Profiler unavailable (PipeWire module-profiler not loaded).");
    return;
  }

  const auto snapOpt = graph->profilerSnapshot();
  if (!snapOpt.has_value() || snapOpt->seq == 0) {
    mvprintw(5, 0, "Waiting for profiler data...");
    return;
  }

  const PwProfilerSnapshot s = *snapOpt;

  auto fmtPct = [](double ratio) -> QString {
    const double pct = ratio * 100.0;
    if (!std::isfinite(pct)) {
      return QStringLiteral("-");
    }
    return QString::number(pct, 'f', pct < 10.0 ? 2 : 1) + "%";
  };

  auto fmtMs = [](const std::optional<double>& ms) -> QString {
    if (!ms.has_value() || !std::isfinite(*ms)) {
      return QStringLiteral("-");
    }
    return QString::number(*ms, 'f', (*ms < 10.0) ? 2 : 1);
  };

  if (s.hasInfo) {
    const QString cpu = QStringLiteral("CPU load: %1 / %2 / %3   XRuns: %4")
                            .arg(fmtPct(s.cpuLoadFast), fmtPct(s.cpuLoadMedium), fmtPct(s.cpuLoadSlow))
                            .arg(s.xrunCount);
    const QByteArray cpuUtf8 = cpu.toUtf8();
    mvaddnstr(5, 0, cpuUtf8.constData(), width);
  } else {
    mvprintw(5, 0, "Profiler info unavailable.");
  }

  if (s.hasClock) {
    QStringList bits;
    if (s.clockDurationMs.has_value()) {
      bits << QStringLiteral("duration %1 ms").arg(fmtMs(s.clockDurationMs));
    }
    if (s.clockDelayMs.has_value()) {
      bits << QStringLiteral("delay %1 ms").arg(fmtMs(s.clockDelayMs));
    }
    if (s.clockXrunDurationMs.has_value() && *s.clockXrunDurationMs > 0.0) {
      bits << QStringLiteral("xrun %1 ms").arg(fmtMs(s.clockXrunDurationMs));
    }
    if (s.clockCycle > 0) {
      bits << QStringLiteral("cycle %1").arg(s.clockCycle);
    }
    const QString clock = QStringLiteral("Clock: %1").arg(bits.join(QStringLiteral(", ")));
    const QByteArray clockUtf8 = clock.toUtf8();
    mvaddnstr(6, 0, clockUtf8.constData(), width);
  }

  mvprintw(8, 0, "Drivers:");

  const int listTop = 10;
  const int listHeight = std::max(0, height - listTop - 2);

  if (s.drivers.isEmpty()) {
    mvprintw(listTop, 0, "(no drivers reported)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, s.drivers.size());

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, static_cast<int>(s.drivers.size()) - listHeight));

  mvprintw(listTop - 1, 0, "  ID   LAT(ms)  BUSY    WAIT   XRUN  NAME");

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= s.drivers.size()) {
      break;
    }
    const auto& d = s.drivers[idx];

    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString lat = fmtMs(d.latencyMs);
    const QString busy = d.busyRatio.has_value() ? fmtPct(*d.busyRatio) : QStringLiteral("-");
    const QString wait = d.waitRatio.has_value() ? fmtPct(*d.waitRatio) : QStringLiteral("-");
    const QString name = d.name.isEmpty() ? QStringLiteral("(unnamed)") : d.name;

    const int y = listTop + row;
    mvprintw(y,
             0,
             "%c %4u  %7s  %6s  %6s  %4d  ",
             selected ? '>' : ' ',
             d.id,
             lat.toUtf8().constData(),
             busy.toUtf8().constData(),
             wait.toUtf8().constData(),
             d.xrunCount);

    const QByteArray nameUtf8 = name.toUtf8();
    const int nameX = 0 + 1 + 5 + 2 + 8 + 2 + 7 + 2 + 7 + 2 + 5 + 2;
    mvaddnstr(y, nameX, nameUtf8.constData(), std::max(0, width - nameX - 1));

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

void drawEnginePage(const QList<SystemdUnitStatus>& units, int& selectedIdx, const QString& engineStatus, int height, int width)
{
  mvprintw(3, 0, "Engine control (systemd --user)");
  mvprintw(4, 0, "Up/Down select  S start  T stop  R restart  g refresh");

  if (!engineStatus.trimmed().isEmpty()) {
    const QByteArray utf8 = engineStatus.toUtf8();
    mvaddnstr(5, 0, utf8.constData(), std::max(0, width - 1));
  }

  const int listTop = 7;
  const int listHeight = std::max(0, height - listTop - 4);
  const int count = units.size();
  if (count <= 0) {
    mvprintw(listTop, 0, "(no units)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, count);

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, count - listHeight));

  mvprintw(listTop - 1, 0, "  STATE          UNIT  DETAILS");

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= count) {
      break;
    }

    const auto& st = units[idx];
    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString active = st.activeState.isEmpty() ? QStringLiteral("-") : st.activeState;
    const QString sub = st.subState.isEmpty() ? QStringLiteral("-") : st.subState;
    const QString details = st.error.isEmpty() ? st.description : QStringLiteral("ERR: %1").arg(st.error);
    const QString line = QStringLiteral("%1/%2  %3  %4").arg(active, sub, st.unit, details);
    const QByteArray utf8 = line.toUtf8();

    const int y = listTop + row;
    mvprintw(y, 0, "%c ", selected ? '>' : ' ');
    mvaddnstr(y, 2, utf8.constData(), std::max(0, width - 3));

    if (selected) {
      attroff(A_REVERSE);
    }
  }

  if (height >= 3) {
    const SystemdUnitStatus st = units.value(selectedIdx);
    const QString active = st.activeState.isEmpty() ? QStringLiteral("-") : st.activeState;
    const QString sub = st.subState.isEmpty() ? QStringLiteral("-") : st.subState;
    const QString load = st.loadState.isEmpty() ? QStringLiteral("-") : st.loadState;
    const QString msg = QStringLiteral("Selected: %1  load:%2  active:%3/%4").arg(st.unit, load, active, sub);
    const QByteArray utf8 = msg.toUtf8();
    mvaddnstr(height - 2, 0, utf8.constData(), std::max(0, width - 1));
  }
}

} // namespace

int main(int argc, char** argv)
{
  setlocale(LC_ALL, "");
  pw_init(&argc, &argv);

  int exitCode = 0;
  {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Headroom"));
    QCoreApplication::setOrganizationName(QStringLiteral("maxheadroom"));
    QCoreApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));

    const QStringList args = app.arguments();
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

        QObject::connect(&recorder, &AudioRecorder::errorOccurred, &app, [&](const QString& msg) {
          recordingStatus = QStringLiteral("Error: %1").arg(msg);
          beep();
        });

        bool running = true;
        bool showHelp = false;
        QString globalStatus;
        QList<SystemdUnitStatus> engineUnits;
        QString engineStatus;
        QElapsedTimer engineRefresh;
        engineRefresh.start();
        bool engineDirty = true;
        while (running) {
          QCoreApplication::processEvents();

          if (recorder.isRecording() && recordingDurationLimitSec > 0) {
            const uint32_t sr = recorder.sampleRate();
            const uint64_t frames = recorder.framesCaptured();
            const double secs = (sr > 0) ? (static_cast<double>(frames) / static_cast<double>(sr)) : 0.0;
            if (secs >= static_cast<double>(recordingDurationLimitSec)) {
              const uint64_t bytes = recorder.dataBytesWritten();
              const QString path = recorder.filePath().isEmpty() ? recordingPath : recorder.filePath();
              recorder.stop();
              recordingStatus = QStringLiteral("Stopped (timer): %1 (%2 bytes)%3")
                                    .arg(path)
                                    .arg(static_cast<qulonglong>(bytes))
                                  .arg(bytes == 0 ? QStringLiteral("  (no audio received; is a session manager/driver running?)") : QString{});
            beep();
          }
        }

        if (page == Page::Engine && (engineDirty || engineRefresh.elapsed() > 1000)) {
          engineUnits.clear();
          engineStatus.clear();

          const QStringList units = EngineControl::defaultUserUnits();
          QString err;
          if (!EngineControl::isSystemctlAvailable()) {
            engineStatus = QStringLiteral("systemctl not found (Engine controls unavailable).");
          } else if (!EngineControl::canTalkToUserSystemd(&err)) {
            engineStatus = QStringLiteral("systemd --user unavailable: %1").arg(err);
          }

          for (const auto& unit : units) {
            engineUnits.append(EngineControl::queryUserUnit(unit));
          }
          selectedEngine = clampIndex(selectedEngine, engineUnits.size());

          engineDirty = false;
          engineRefresh.restart();
        }

        const int ch = getch();
        if (ch != ERR) {
          if (showHelp) {
            switch (ch) {
            case 27: // Esc
            case '\n':
            case KEY_ENTER:
            case 'q':
            case 'Q':
            case '?':
            case 'h':
            case 'H':
              showHelp = false;
              break;
            default:
              break;
            }
          } else if (ch == '?' || ch == 'h' || ch == 'H') {
            showHelp = true;
          } else {
          switch (ch) {
          case 'q':
          case 'Q':
            running = false;
            break;
          case '\t': {
            const int next = static_cast<int>(page) + 1;
            page = pageFromIndex(next);
            break;
          }
          case KEY_F(1):
          case '1':
            page = Page::Outputs;
            break;
          case KEY_F(2):
          case '2':
            page = Page::Inputs;
            break;
          case KEY_F(3):
          case '3':
            page = Page::Streams;
            break;
          case KEY_F(4):
          case '4':
            page = Page::Patchbay;
            break;
          case KEY_F(5):
          case '5':
            page = Page::Eq;
            break;
          case KEY_F(6):
          case '6':
            page = Page::Recording;
            break;
          case KEY_F(7):
          case '7':
            page = Page::Status;
            break;
          case KEY_F(8):
          case '8':
            page = Page::Engine;
            break;
          default:
            break;
          }

          if (page == Page::Outputs || page == Page::Inputs || page == Page::Streams) {
            QList<PwNodeInfo> devices;
            int* selPtr = nullptr;

            if (page == Page::Outputs) {
              QSettings s;
              devices = applySinksOrder(graph.audioSinks(), s);
              selPtr = &selectedSink;
            } else if (page == Page::Inputs) {
              devices = graph.audioSources();
              selPtr = &selectedSource;
            } else {
              devices = graph.audioPlaybackStreams();
              devices.append(graph.audioCaptureStreams());
              std::sort(devices.begin(), devices.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
                if (a.mediaClass != b.mediaClass) {
                  return a.mediaClass < b.mediaClass;
                }
                return a.description < b.description;
              });
              selPtr = &selectedStream;
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
                globalStatus = QStringLiteral("Default device controls unavailable.");
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
                globalStatus = sink ? QStringLiteral("Failed to set default output.") : QStringLiteral("Failed to set default input.");
                beep();
                return;
              }

              globalStatus = sink ? QStringLiteral("Default output: %1").arg(displayNameForNode(selectedNode))
                                  : QStringLiteral("Default input: %1").arg(displayNameForNode(selectedNode));
            };

            auto doMoveStream = [&]() {
              if (page != Page::Streams || nodeId == 0u) {
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
              if (page == Page::Outputs && !selectedNode.name.isEmpty()) {
                const int delta = (ch == '[') ? -1 : 1;
                const QString selectedName = selectedNode.name;
                const QList<PwNodeInfo> sinksNow = graph.audioSinks();
                QString statusMsg;
                const bool ok = moveSinkInOrder(sinksNow, selectedName, delta, &statusMsg);
                if (!statusMsg.isEmpty()) {
                  globalStatus = statusMsg;
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
              if (page == Page::Outputs) {
                trySetDefault(true);
              } else if (page == Page::Inputs) {
                trySetDefault(false);
              } else if (page == Page::Streams) {
                doMoveStream();
              } else {
                beep();
              }
              break;
            case 't':
            case 'T':
              if (page == Page::Streams) {
                doMoveStream();
              }
              break;
            default:
              break;
            }
          } else if (page == Page::Patchbay) {
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

            QSettings portSettings;
            const QList<PwNodeInfo> nodesAll = graph.nodes();
            const QList<PwPortInfo> portsAll = graph.ports();

            QHash<uint32_t, PwNodeInfo> nodesByIdAll;
            nodesByIdAll.reserve(nodesAll.size());
            for (const auto& n : nodesAll) {
              nodesByIdAll.insert(n.id, n);
            }

            QHash<uint32_t, PwPortInfo> portsByIdAll;
            portsByIdAll.reserve(portsAll.size());
            for (const auto& p : portsAll) {
              portsByIdAll.insert(p.id, p);
            }

            auto portLocked = [&](uint32_t portId) -> bool {
              const auto it = portsByIdAll.find(portId);
              if (it == portsByIdAll.end()) {
                return false;
              }
              const PwPortInfo port = it.value();
              const PwNodeInfo node = nodesByIdAll.value(port.nodeId, PwNodeInfo{});
              return PatchbayPortConfigStore::isLocked(portSettings, node.name, port.name);
            };

            selectedLink = clampIndex(selectedLink, links.size());

            switch (ch) {
            case KEY_UP:
              selectedLink = clampIndex(selectedLink - 1, links.size());
              break;
            case KEY_DOWN:
              selectedLink = clampIndex(selectedLink + 1, links.size());
              break;
            case 'd':
            case 'D':
            case KEY_DC:
            case 127: // backspace/delete on some terminals
              if (!links.isEmpty()) {
                const PwLinkInfo link = links[selectedLink];
                if (portLocked(link.outputPortId) || portLocked(link.inputPortId)) {
                  patchbayStatus = QStringLiteral("Cannot delete: port locked.");
                  beep();
                } else {
                  const bool ok = graph.destroyLink(link.id);
                  if (!ok) {
                    patchbayStatus = QStringLiteral("Delete failed for link %1.").arg(link.id);
                    beep();
                  } else {
                    patchbayStatus = QStringLiteral("Deleted link %1.").arg(link.id);
                  }
                }
              } else {
                beep();
              }
              break;
            case 'c':
            case 'C':
            case '\n':
            case KEY_ENTER: {
              int height = 0;
              int width = 0;
              getmaxyx(stdscr, height, width);

              const QList<PwNodeInfo> nodes = graph.nodes();
              const QList<PwPortInfo> ports = graph.ports();

              QHash<uint32_t, PwNodeInfo> nodesById;
              nodesById.reserve(nodes.size());
              for (const auto& n : nodes) {
                nodesById.insert(n.id, n);
              }

              const QList<PwNodeInfo> outNodes = nodesWithPortDirection(nodes, ports, QStringLiteral("out"));
              const uint32_t outNodeId = promptSelectNodeId("Select output node", outNodes, patchbayOutNodeId, height, width);
              if (outNodeId == 0) {
                break;
              }

              const QList<PwPortInfo> outPorts = portsForNode(ports, outNodeId, QStringLiteral("out"));
              const uint32_t outPortId = promptSelectPortId("Select output port", outPorts, patchbayOutPortId, height, width, nodesById);
              if (outPortId == 0) {
                break;
              }
              if (portLocked(outPortId)) {
                patchbayStatus = QStringLiteral("Cannot connect: output port locked.");
                beep();
                break;
              }

              PortKind wantKind = PortKind::Other;
              for (const auto& p : outPorts) {
                if (p.id == outPortId) {
                  wantKind = portKindFor(p, nodesById);
                  break;
                }
              }

              QSet<uint32_t> allowedInNodes;
              allowedInNodes.reserve(ports.size());
              for (const auto& p : ports) {
                if (p.direction == QStringLiteral("in") && p.nodeId != 0 && portKindFor(p, nodesById) == wantKind) {
                  allowedInNodes.insert(p.nodeId);
                }
              }

              QList<PwNodeInfo> inNodes;
              inNodes.reserve(nodes.size());
              for (const auto& n : nodes) {
                if (allowedInNodes.contains(n.id)) {
                  inNodes.push_back(n);
                }
              }
              std::sort(inNodes.begin(), inNodes.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
                if (a.mediaClass != b.mediaClass) {
                  return a.mediaClass < b.mediaClass;
                }
                if (a.description != b.description) {
                  return a.description < b.description;
                }
                return a.name < b.name;
              });

              if (inNodes.isEmpty()) {
                patchbayStatus = QStringLiteral("No compatible input nodes (need %1).").arg(QString::fromUtf8(portKindLabelShort(wantKind)));
                beep();
                break;
              }

              const QString inTitle = QStringLiteral("Select input node (%1)").arg(QString::fromUtf8(portKindLabelShort(wantKind)));
              const QByteArray inTitleUtf8 = inTitle.toUtf8();
              const uint32_t inNodeId = promptSelectNodeId(inTitleUtf8.constData(), inNodes, patchbayInNodeId, height, width);
              if (inNodeId == 0) {
                break;
              }

              QList<PwPortInfo> inPortsAll = portsForNode(ports, inNodeId, QStringLiteral("in"));
              QList<PwPortInfo> inPorts;
              inPorts.reserve(inPortsAll.size());
              for (const auto& p : inPortsAll) {
                if (portKindFor(p, nodesById) == wantKind) {
                  inPorts.push_back(p);
                }
              }
              if (inPorts.isEmpty()) {
                patchbayStatus = QStringLiteral("No compatible input ports.");
                beep();
                break;
              }

              const uint32_t inPortId = promptSelectPortId("Select input port", inPorts, patchbayInPortId, height, width, nodesById);
              if (inPortId == 0) {
                break;
              }
              if (portLocked(inPortId)) {
                patchbayStatus = QStringLiteral("Cannot connect: input port locked.");
                beep();
                break;
              }

              patchbayOutNodeId = outNodeId;
              patchbayOutPortId = outPortId;
              patchbayInNodeId = inNodeId;
              patchbayInPortId = inPortId;

              bool exists = false;
              for (const auto& l : graph.links()) {
                if (l.outputNodeId == outNodeId && l.outputPortId == outPortId && l.inputNodeId == inNodeId && l.inputPortId == inPortId) {
                  exists = true;
                  break;
                }
              }

              if (exists) {
                patchbayStatus = QStringLiteral("Already connected.");
              } else {
                const bool ok = graph.createLink(outNodeId, outPortId, inNodeId, inPortId);
                if (!ok) {
                  patchbayStatus = QStringLiteral("Connect failed.");
                  beep();
                } else {
                  patchbayStatus = QStringLiteral("Connected.");
                }
              }
              break;
            }
            default:
              break;
            }
          }
          else if (page == Page::Eq) {
            QList<PwNodeInfo> targets = graph.audioSinks();
            targets.append(graph.audioSources());
            const int count = targets.size();

            selectedEqDevice = clampIndex(selectedEqDevice, count);

            const PwNodeInfo selectedNode = targets.isEmpty() ? PwNodeInfo{} : targets[selectedEqDevice];
            const QString nodeName = selectedNode.name;

            switch (ch) {
            case KEY_UP:
              selectedEqDevice = clampIndex(selectedEqDevice - 1, count);
              break;
            case KEY_DOWN:
              selectedEqDevice = clampIndex(selectedEqDevice + 1, count);
              break;
            case 'e':
            case 'E':
              if (!nodeName.isEmpty()) {
                EqPreset p = eq.presetForNodeName(nodeName);
                p.enabled = !p.enabled;
                eq.setPresetForNodeName(nodeName, p);
                eqStatus = QStringLiteral("%1: EQ %2").arg(displayNameForNode(selectedNode), p.enabled ? "enabled" : "disabled");
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
              eqStatus = QStringLiteral("%1: applied preset \"%2\"").arg(displayNameForNode(selectedNode), presets[idx].first);
              break;
            }
            default:
              break;
            }
          }
          else if (page == Page::Engine) {
            const int n = engineUnits.size();
            selectedEngine = clampIndex(selectedEngine, n);

            auto selectedUnit = [&]() -> QString {
              const SystemdUnitStatus st = engineUnits.value(selectedEngine);
              return st.unit;
            };

            auto doStart = [&]() {
              const QString unit = selectedUnit();
              if (unit.isEmpty()) {
                beep();
                globalStatus = QStringLiteral("No unit selected.");
                return;
              }

              QString err;
              const bool ok = EngineControl::startUserUnit(unit, &err);
              if (!ok) {
                globalStatus = err.isEmpty() ? QStringLiteral("Start failed.") : QStringLiteral("Start failed: %1").arg(err);
                beep();
                return;
              }
              globalStatus = QStringLiteral("Started: %1").arg(unit);
              engineDirty = true;
            };

            auto doStop = [&]() {
              const QString unit = selectedUnit();
              if (unit.isEmpty()) {
                beep();
                globalStatus = QStringLiteral("No unit selected.");
                return;
              }

              QString err;
              const bool ok = EngineControl::stopUserUnit(unit, &err);
              if (!ok) {
                globalStatus = err.isEmpty() ? QStringLiteral("Stop failed.") : QStringLiteral("Stop failed: %1").arg(err);
                beep();
                return;
              }
              globalStatus = QStringLiteral("Stopped: %1").arg(unit);
              engineDirty = true;
            };

            auto doRestart = [&]() {
              const QString unit = selectedUnit();
              if (unit.isEmpty()) {
                beep();
                globalStatus = QStringLiteral("No unit selected.");
                return;
              }

              QString err;
              const bool ok = EngineControl::restartUserUnit(unit, &err);
              if (!ok) {
                globalStatus = err.isEmpty() ? QStringLiteral("Restart failed.") : QStringLiteral("Restart failed: %1").arg(err);
                beep();
                return;
              }
              globalStatus = QStringLiteral("Restarted: %1").arg(unit);
              engineDirty = true;
            };

            switch (ch) {
            case KEY_UP:
              selectedEngine = clampIndex(selectedEngine - 1, n);
              break;
            case KEY_DOWN:
              selectedEngine = clampIndex(selectedEngine + 1, n);
              break;
            case 'g':
            case 'G':
              engineDirty = true;
              globalStatus = QStringLiteral("Engine: refresh requested.");
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
          }
          else if (page == Page::Recording) {
            const QVector<RecordingTarget> targets = buildRecordingTargets(&graph);
            selectedRecordingTarget = clampIndex(selectedRecordingTarget, targets.size());

            switch (ch) {
            case KEY_UP:
              selectedRecordingTarget = clampIndex(selectedRecordingTarget - 1, targets.size());
              break;
            case KEY_DOWN:
              selectedRecordingTarget = clampIndex(selectedRecordingTarget + 1, targets.size());
              break;
            case 'o':
            case 'O': {
              if (recorder.isRecording()) {
                beep();
                recordingStatus = QStringLiteral("Stop recording before changing format.");
                break;
              }
              recordingFormat = (recordingFormat == AudioRecorder::Format::Wav) ? AudioRecorder::Format::Flac : AudioRecorder::Format::Wav;
              recordingStatus = QStringLiteral("Format set: %1").arg(AudioRecorder::formatToString(recordingFormat));
              break;
            }
            case 't':
            case 'T': {
              if (recorder.isRecording()) {
                beep();
                recordingStatus = QStringLiteral("Stop recording before changing the timer.");
                break;
              }

              int height = 0;
              int width = 0;
              getmaxyx(stdscr, height, width);
              const QString cur = QString::number(std::max(0, recordingDurationLimitSec));
              const QString next = promptInputLine("Recording timer", "Stop after N seconds (0=off).", cur, height, width);
              if (next.isEmpty()) {
                break;
              }
              bool ok = false;
              const int v = next.toInt(&ok);
              if (!ok || v < 0) {
                beep();
                recordingStatus = QStringLiteral("Invalid timer value.");
                break;
              }
              recordingDurationLimitSec = v;
              recordingStatus = (recordingDurationLimitSec > 0) ? QStringLiteral("Timer set: %1 sec").arg(recordingDurationLimitSec)
                                                               : QStringLiteral("Timer disabled.");
              break;
            }
            case 'f':
            case 'F': {
              if (recorder.isRecording()) {
                beep();
                recordingStatus = QStringLiteral("Stop recording before changing the file path.");
                break;
              }

              int height = 0;
              int width = 0;
              getmaxyx(stdscr, height, width);
              const QString next =
                  promptInputLine("Recording output", "Enter output path or template (.wav/.flac; supports {datetime} {target} {ext}).", recordingPath, height, width);
              if (!next.isEmpty()) {
                recordingPath = next;
                recordingStatus = QStringLiteral("File set: %1").arg(recordingPath);
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
                recordingStatus = QStringLiteral("Stopped: %1 (%2 bytes)%3")
                                      .arg(recorder.filePath().isEmpty() ? recordingPath : recorder.filePath())
                                      .arg(static_cast<qulonglong>(bytes))
                                      .arg(bytes == 0 ? QStringLiteral("  (no audio received; is a session manager/driver running?)") : QString{});
              } else {
                const RecordingTarget t = targets.value(selectedRecordingTarget, RecordingTarget{});
                const QString resolvedPath = AudioRecorder::expandPathTemplate(recordingPath, t.label, recordingFormat);
                const RecordingGraphSnapshot snap = captureRecordingSnapshot(&graph);
                AudioRecorder::StartOptions o;
                o.filePath = resolvedPath;
                o.targetObject = t.targetObject;
                o.captureSink = t.captureSink;
                o.format = recordingFormat;
                const bool ok = recorder.start(o);
                if (!ok) {
                  const QString err = recorder.lastError();
                  recordingStatus = err.isEmpty() ? QStringLiteral("Start failed.") : QStringLiteral("Start failed: %1").arg(err);
                  beep();
                } else {
                  recordingStatus = QStringLiteral("Recording started.");
                  recordingSnapshot = snap;
                }
              }
              break;
            }
            case 'm':
            case 'M': {
              int height = 0;
              int width = 0;
              getmaxyx(stdscr, height, width);
              if (!recordingSnapshot) {
                beep();
                recordingStatus = QStringLiteral("No metadata snapshot yet (start a recording first).");
                break;
              }
              promptScrollLines("Recording metadata", recordingSnapshotLines(*recordingSnapshot), height, width);
              break;
            }
            default:
              break;
            }
          }
          else if (page == Page::Status) {
            const auto snapOpt = graph.profilerSnapshot();
            const int n = snapOpt.has_value() ? snapOpt->drivers.size() : 0;
            switch (ch) {
            case KEY_UP:
              selectedStatus = clampIndex(selectedStatus - 1, n);
              break;
            case KEY_DOWN:
              selectedStatus = clampIndex(selectedStatus + 1, n);
              break;
            default:
              break;
            }
          }
        }
        }

        int height = 0;
        int width = 0;
        getmaxyx(stdscr, height, width);

        erase();
        drawHeader(page, width);

        switch (page) {
        case Page::Outputs: {
          QSettings s;
          const QList<PwNodeInfo> sinks = applySinksOrder(graph.audioSinks(), s);
          drawListPage("Output Devices", sinks, &graph, selectedSink, graph.defaultAudioSinkId(), height, width);
          break;
        }
        case Page::Inputs: {
          const QList<PwNodeInfo> sources = graph.audioSources();
          drawListPage("Input Devices", sources, &graph, selectedSource, graph.defaultAudioSourceId(), height, width);
          break;
        }
        case Page::Streams:
          drawStreamsPage(&graph, selectedStream, height, width);
          break;
        case Page::Patchbay: {
          drawPatchbayPage(&graph, selectedLink, patchbayStatus, height, width);
          break;
        }
        case Page::Eq:
          drawEqPage(&graph, &eq, selectedEqDevice, eqStatus, height, width);
          break;
        case Page::Recording:
          drawRecordingPage(&graph,
                            &recorder,
                            recordingSnapshot ? &*recordingSnapshot : nullptr,
                            selectedRecordingTarget,
                            recordingPath,
                            recordingFormat,
                            recordingDurationLimitSec,
                            recordingStatus,
                            height,
                            width);
          break;
        case Page::Status:
          drawStatusPage(&graph, selectedStatus, height, width);
          break;
        case Page::Engine:
          drawEnginePage(engineUnits, selectedEngine, engineStatus, height, width);
          break;
        }

        auto fmtVolPct = [](float v) -> QString {
          const int pct = static_cast<int>(std::round(v * 100.0f));
          return QStringLiteral("%1%").arg(pct);
        };

        auto fmtMute = [](bool mute) -> QString {
          return mute ? QStringLiteral("muted") : QStringLiteral("unmuted");
        };

        auto nodeSummary = [&](const QString& kind, const PwNodeInfo& node, const PwNodeControls& c, bool isDefault) -> QString {
          if (node.id == 0u) {
            return QStringLiteral("%1: (none)").arg(kind);
          }
          const QString def = isDefault ? QStringLiteral(" (default)") : QString();
          return QStringLiteral("%1 %2  %3  %4  %5%6")
              .arg(kind)
              .arg(node.id)
              .arg(displayNameForNode(node))
              .arg(fmtVolPct(c.volume))
              .arg(fmtMute(c.mute))
              .arg(def);
        };

        QString statusLine;
        switch (page) {
        case Page::Outputs: {
          QSettings s;
          const QList<PwNodeInfo> sinks = applySinksOrder(graph.audioSinks(), s);
          const PwNodeInfo n = sinks.value(clampIndex(selectedSink, sinks.size()));
          const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
          const bool isDef = graph.defaultAudioSinkId().has_value() && graph.defaultAudioSinkId().value() == n.id;
          statusLine = nodeSummary(QStringLiteral("OUT"), n, c, isDef);
          break;
        }
        case Page::Inputs: {
          const QList<PwNodeInfo> sources = graph.audioSources();
          const PwNodeInfo n = sources.value(clampIndex(selectedSource, sources.size()));
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
          const PwNodeInfo n = streams.value(clampIndex(selectedStream, streams.size()));
          const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
          const bool isPlayback = n.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));
          const StreamRoute route = (n.id != 0u) ? routeForStream(&graph, n) : StreamRoute{};
          const QString routeText = route.deviceName.isEmpty()
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
          const PwLinkInfo l = links.value(clampIndex(selectedLink, links.size()));
          if (l.id != 0u) {
            statusLine = QStringLiteral("Link %1  out:%2/%3  in:%4/%5")
                             .arg(l.id)
                             .arg(l.outputNodeId)
                             .arg(l.outputPortId)
                             .arg(l.inputNodeId)
                             .arg(l.inputPortId);
          } else {
            statusLine = QStringLiteral("Patchbay: (no links)");
          }
          if (!patchbayStatus.trimmed().isEmpty()) {
            statusLine += QStringLiteral("  |  %1").arg(patchbayStatus);
          }
          break;
        }
        case Page::Eq: {
          QList<PwNodeInfo> targets = graph.audioSinks();
          targets.append(graph.audioSources());
          const PwNodeInfo n = targets.value(clampIndex(selectedEqDevice, targets.size()));
          const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
          const EqPreset p = n.name.isEmpty() ? EqPreset{} : eq.presetForNodeName(n.name);
          statusLine = nodeSummary(QStringLiteral("EQ"), n, c, false);
          if (n.id != 0u) {
            statusLine += QStringLiteral("  |  %1").arg(p.enabled ? QStringLiteral("ON") : QStringLiteral("OFF"));
          }
          if (!eqStatus.trimmed().isEmpty()) {
            statusLine += QStringLiteral("  |  %1").arg(eqStatus);
          }
          break;
        }
        case Page::Recording: {
          const QString state = recorder.isRecording() ? QStringLiteral("Recording") : QStringLiteral("Idle");
          statusLine = QStringLiteral("%1  |  %2").arg(QStringLiteral("REC")).arg(state);
          if (!recordingStatus.trimmed().isEmpty()) {
            statusLine += QStringLiteral("  |  %1").arg(recordingStatus);
          }
          break;
        }
        case Page::Engine: {
          const SystemdUnitStatus st = engineUnits.value(clampIndex(selectedEngine, engineUnits.size()));
          if (!engineStatus.trimmed().isEmpty()) {
            statusLine = QStringLiteral("Engine: %1").arg(engineStatus);
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

        if (!globalStatus.trimmed().isEmpty()) {
          statusLine += QStringLiteral("  |  %1").arg(globalStatus);
        }
        drawStatusBar(statusLine, height, width);

        if (showHelp) {
          drawHelpOverlay(page, height, width);
        }

        refresh();
      }

      endwin();
    }
  }
  }

  pw_deinit();
  return exitCode;
}
