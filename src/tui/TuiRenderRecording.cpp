#include "tui/TuiInternal.h"

#include <algorithm>
#include <cmath>

#include <curses.h>

namespace headroomtui {

static QString formatDuration(double seconds)
{
  if (!std::isfinite(seconds) || seconds < 0.0) {
    seconds = 0.0;
  }
  const int total = static_cast<int>(seconds + 0.5);
  const int mm = total / 60;
  const int ss = total % 60;
  return QStringLiteral("%1:%2").arg(mm, 2, 10, QLatin1Char('0')).arg(ss, 2, 10, QLatin1Char('0'));
}

static QString describeRecorderTarget(const AudioRecorder* recorder, const QVector<RecordingTarget>& targets)
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

} // namespace headroomtui

