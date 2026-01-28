#include "tui/TuiInternal.h"

#include "backend/EqManager.h"

#include <algorithm>
#include <cmath>

#include <curses.h>

namespace headroomtui {

static const char* eqBandTypeShort(EqBandType t)
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

static QString summarizeEqPreset(const EqPreset& preset)
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

  const QList<PwNodeInfo> targets = eqTargetsForGraph(graph);
  int sinkCount = 0;
  int sourceCount = 0;
  int playbackCount = 0;
  int captureCount = 0;
  for (const auto& n : targets) {
    if (n.mediaClass == QStringLiteral("Audio/Sink")) {
      ++sinkCount;
    } else if (n.mediaClass == QStringLiteral("Audio/Source")) {
      ++sourceCount;
    } else if (n.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
      ++playbackCount;
    } else if (n.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
      ++captureCount;
    }
  }

  mvprintw(4,
           0,
           "Targets: %d  (Outputs: %d, Inputs: %d, App PB: %d, App REC: %d)",
           static_cast<int>(targets.size()),
           sinkCount,
           sourceCount,
           playbackCount,
           captureCount);
  mvprintw(5, 0, "Up/Down select  e toggle  p/Enter preset");

  if (!statusLine.isEmpty()) {
    const QByteArray statusUtf8 = statusLine.toUtf8();
    mvaddnstr(6, 0, statusUtf8.constData(), std::max(0, width - 1));
  }

  const int count = static_cast<int>(targets.size());
  const int listTop = 8;
  const int listHeight = std::max(0, height - listTop - 4);

  if (count <= 0) {
    mvprintw(listTop, 0, "(no targets)");
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

    const char* kind = "UNK";
    if (node.mediaClass == QStringLiteral("Audio/Sink")) {
      kind = "OUT";
    } else if (node.mediaClass == QStringLiteral("Audio/Source")) {
      kind = "IN ";
    } else if (node.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
      kind = "PB ";
    } else if (node.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
      kind = "REC";
    }
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

} // namespace headroomtui

