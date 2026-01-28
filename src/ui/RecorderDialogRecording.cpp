#include "RecorderDialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>

#include "backend/AudioRecorder.h"
#include "backend/PipeWireGraph.h"

namespace RecorderDialogInternal {

QString nodeDisplayName(const PwNodeInfo& n);

} // namespace RecorderDialogInternal

namespace {

struct RecordingMetadataSnapshot final {
  QString summary;
  QString details;
};

QString snapshotNodeLine(const PwNodeInfo& n)
{
  const QString base = RecorderDialogInternal::nodeDisplayName(n);
  const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
  const QString label = (!app.isEmpty() && app != base) ? QStringLiteral("%1 — %2").arg(app, base) : base;

  return QStringLiteral("%1  %2  (%3)")
      .arg(static_cast<qulonglong>(n.id), 4)
      .arg(label)
      .arg(n.mediaClass.isEmpty() ? QStringLiteral("unknown") : n.mediaClass);
}

QString idOrUnknown(std::optional<uint32_t> v)
{
  return v ? QString::number(*v) : QStringLiteral("(unknown)");
}

RecordingMetadataSnapshot captureRecordingMetadata(PipeWireGraph* graph, const QString& targetLabel, const QString& targetObject, bool captureSink)
{
  RecordingMetadataSnapshot snap;
  const QString capturedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  if (!graph) {
    snap.summary = QObject::tr("snapshot: (graph unavailable)");
    snap.details = QObject::tr("Captured at (UTC): %1\nTarget: %2\nTarget object: %3\nCapture sink: %4\n\n(graph unavailable)")
                       .arg(capturedAtUtc, targetLabel, targetObject, captureSink ? QObject::tr("yes") : QObject::tr("no"));
    return snap;
  }

  const QList<PwNodeInfo> sinks = graph->audioSinks();
  const QList<PwNodeInfo> sources = graph->audioSources();
  const QList<PwNodeInfo> playback = graph->audioPlaybackStreams();
  const QList<PwNodeInfo> capture = graph->audioCaptureStreams();

  const std::optional<uint32_t> defSink = graph->defaultAudioSinkId();
  const std::optional<uint32_t> defSource = graph->defaultAudioSourceId();

  std::optional<PwNodeInfo> targetNode = std::nullopt;
  if (!targetObject.isEmpty()) {
    for (const auto& n : graph->nodes()) {
      if (n.name == targetObject) {
        targetNode = n;
        break;
      }
    }
  }

  snap.summary = QObject::tr("snapshot: %1 out, %2 in, %3 playback, %4 capture")
                     .arg(sinks.size())
                     .arg(sources.size())
                     .arg(playback.size())
                     .arg(capture.size());

  QString details;
  details += QObject::tr("Captured at (UTC): %1\n").arg(capturedAtUtc);
  details += QObject::tr("Target: %1\n").arg(targetLabel);
  details += QObject::tr("Target object: %1\n").arg(targetObject.isEmpty() ? QStringLiteral("(none)") : targetObject);
  details += QObject::tr("Capture sink: %1\n").arg(captureSink ? QObject::tr("yes") : QObject::tr("no"));
  details += QObject::tr("Default sink id: %1\n").arg(idOrUnknown(defSink));
  details += QObject::tr("Default source id: %1\n").arg(idOrUnknown(defSource));
  if (targetNode) {
    details += QObject::tr("Target node id: %1\n").arg(QString::number(targetNode->id));
    details += QObject::tr("Target media class: %1\n").arg(targetNode->mediaClass.isEmpty() ? QStringLiteral("unknown") : targetNode->mediaClass);
  }

  details += QLatin1String("\n");
  details += QObject::tr("Output devices (%1):\n").arg(sinks.size());
  for (const auto& n : sinks) {
    details += QStringLiteral("  %1\n").arg(snapshotNodeLine(n));
  }

  details += QLatin1String("\n");
  details += QObject::tr("Input devices (%1):\n").arg(sources.size());
  for (const auto& n : sources) {
    details += QStringLiteral("  %1\n").arg(snapshotNodeLine(n));
  }

  details += QLatin1String("\n");
  details += QObject::tr("Playback streams (%1):\n").arg(playback.size());
  for (const auto& n : playback) {
    details += QStringLiteral("  %1\n").arg(snapshotNodeLine(n));
  }

  details += QLatin1String("\n");
  details += QObject::tr("Capture streams (%1):\n").arg(capture.size());
  for (const auto& n : capture) {
    details += QStringLiteral("  %1\n").arg(snapshotNodeLine(n));
  }

  snap.details = details.trimmed();
  return snap;
}

double durationSeconds(uint64_t frames, uint32_t sampleRate)
{
  if (sampleRate == 0) {
    return 0.0;
  }
  return static_cast<double>(frames) / static_cast<double>(sampleRate);
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

} // namespace

void RecorderDialog::syncUi()
{
  const bool recording = m_recorder && m_recorder->isRecording();
  const uint32_t rate = m_recorder ? m_recorder->sampleRate() : 0u;
  const uint32_t ch = m_recorder ? m_recorder->channels() : 0u;
  const uint32_t quantum = m_recorder ? m_recorder->quantumFrames() : 0u;
  const uint64_t bytes = m_recorder ? m_recorder->dataBytesWritten() : 0u;
  const uint64_t frames = m_recorder ? m_recorder->framesCaptured() : 0u;
  const float peakDb = m_recorder ? m_recorder->peakDb() : -100.0f;
  const float rmsDb = m_recorder ? m_recorder->rmsDb() : -100.0f;
  const QString state = recording ? tr("Recording (%1)").arg(m_recorder->streamStateString()) : tr("Idle");

  m_stateLabel->setText(state);

  const auto uiFmt = AudioRecorder::formatFromString(m_formatCombo ? m_formatCombo->currentData().toString() : QString{}).value_or(AudioRecorder::Format::Wav);
  const auto fmt = (recording && m_recorder) ? m_recorder->format() : uiFmt;
  const QString fmtStr = (fmt == AudioRecorder::Format::Wav) ? tr("WAV f32") : tr("FLAC pcm24");

  const double secs = durationSeconds(frames, rate == 0 ? 48000u : rate);
  m_formatLabel->setText(tr("%1 • %2 Hz • %3 ch • %4")
                             .arg(fmtStr)
                             .arg(rate == 0 ? 48000 : static_cast<int>(rate))
                             .arg(ch == 0 ? 2 : static_cast<int>(ch))
                             .arg(formatDuration(secs)));
  m_bytesLabel->setText(tr("%1 frames • %2 bytes (raw)").arg(static_cast<qulonglong>(frames)).arg(static_cast<qulonglong>(bytes)));
  m_levelsLabel->setText(recording ? tr("Peak %1 dBFS • RMS %2 dBFS").arg(peakDb, 0, 'f', 1).arg(rmsDb, 0, 'f', 1) : tr("-"));

  if (m_metadataLabel) {
    const QString q = quantum > 0 ? QString::number(quantum) : QStringLiteral("-");
    const QString snap = m_hasMetadataSnapshot ? m_metadataSnapshotSummary : tr("(not captured yet)");
    m_metadataLabel->setText(tr("quantum %1 frames • %2").arg(q, snap));
  }
  if (m_metadataButton) {
    m_metadataButton->setEnabled(m_hasMetadataSnapshot);
  }

  const QString tmpl = m_fileEdit ? m_fileEdit->text().trimmed() : QString{};
  const QString targetLabel = m_targetCombo ? m_targetCombo->currentText() : QString{};
  const QString preview = recording ? (m_recorder ? m_recorder->filePath() : QString{}) : AudioRecorder::expandPathTemplate(tmpl, targetLabel, uiFmt);
  m_previewLabel->setText(preview.isEmpty() ? tr("-") : preview);

  const QString err = m_recorder ? m_recorder->lastError() : QString{};
  m_errorLabel->setText(err.isEmpty() ? tr("(none)") : err);

  m_startStopButton->setText(recording ? tr("Stop") : tr("Start"));
  m_targetCombo->setEnabled(!recording);
  m_formatCombo->setEnabled(!recording);
  m_durationSpin->setEnabled(!recording);
  m_fileEdit->setEnabled(!recording);
  m_browseButton->setEnabled(!recording);
}

void RecorderDialog::browseFile()
{
  const QString current = m_fileEdit->text().trimmed();
  const QString startDir = current.isEmpty() ? QDir::homePath() : QFileInfo(current).absolutePath();
  const QString picked =
      QFileDialog::getSaveFileName(this, tr("Choose Output File"), startDir, tr("Audio files (*.wav *.flac);;WAV files (*.wav);;FLAC files (*.flac);;All files (*)"));
  if (picked.trimmed().isEmpty()) {
    return;
  }

  const auto fmt = AudioRecorder::formatFromString(m_formatCombo->currentData().toString()).value_or(AudioRecorder::Format::Wav);
  const QString adjusted = AudioRecorder::ensureFileExtension(picked, fmt);
  m_fileEdit->setText(adjusted);

  QSettings s;
  s.beginGroup(QStringLiteral("recording"));
  s.setValue(QStringLiteral("filePath"), adjusted);
  s.endGroup();
}

void RecorderDialog::startStop()
{
  if (!m_recorder) {
    return;
  }

  if (m_recorder->isRecording()) {
    m_recorder->stop();
    syncUi();
    return;
  }

  const QString templateOrPath = m_fileEdit->text().trimmed();
  QString targetObject;
  bool captureSink = true;
  {
    const QVariant v = m_targetCombo->currentData();
    if (v.isValid()) {
      const QVariantMap d = v.toMap();
      if (d.contains(QStringLiteral("targetObject")) && d.contains(QStringLiteral("captureSink"))) {
        targetObject = d.value(QStringLiteral("targetObject")).toString();
        captureSink = d.value(QStringLiteral("captureSink")).toBool();
      }
    }
  }

  const auto format = AudioRecorder::formatFromString(m_formatCombo->currentData().toString()).value_or(AudioRecorder::Format::Wav);
  const int durationSec = std::max(0, m_durationSpin->value());
  const QString targetLabel = m_targetCombo->currentText();
  const QString resolvedPath = AudioRecorder::expandPathTemplate(templateOrPath, targetLabel, format);
  const RecordingMetadataSnapshot snap = captureRecordingMetadata(m_graph, targetLabel, targetObject, captureSink);

  QSettings s;
  s.beginGroup(QStringLiteral("recording"));
  s.setValue(QStringLiteral("filePath"), templateOrPath);
  s.setValue(QStringLiteral("targetObject"), targetObject);
  s.setValue(QStringLiteral("captureSink"), captureSink);
  s.setValue(QStringLiteral("format"), AudioRecorder::formatToString(format));
  s.setValue(QStringLiteral("durationSec"), durationSec);
  s.endGroup();

  AudioRecorder::StartOptions o;
  o.filePath = resolvedPath;
  o.targetObject = targetObject;
  o.captureSink = captureSink;
  o.format = format;

  const bool ok = m_recorder->start(o);
  if (!ok) {
    m_hasMetadataSnapshot = false;
    m_metadataSnapshotSummary.clear();
    m_metadataSnapshotDetails.clear();
    syncUi();
    return;
  }

  m_hasMetadataSnapshot = true;
  m_metadataSnapshotSummary = snap.summary;
  m_metadataSnapshotDetails = snap.details;

  // Auto-stop timer lives on the recorder so it keeps working even if the dialog is closed.
  QTimer* autoStopTimer = m_recorder->findChild<QTimer*>(QStringLiteral("headroomRecorderAutoStopTimer"));
  if (!autoStopTimer) {
    autoStopTimer = new QTimer(m_recorder);
    autoStopTimer->setObjectName(QStringLiteral("headroomRecorderAutoStopTimer"));
    autoStopTimer->setSingleShot(true);
    QObject::connect(autoStopTimer, &QTimer::timeout, m_recorder, &AudioRecorder::stop);
  }
  autoStopTimer->stop();
  if (durationSec > 0) {
    autoStopTimer->start(durationSec * 1000);
  }

  syncUi();
}

void RecorderDialog::showMetadata()
{
  if (!m_hasMetadataSnapshot) {
    QMessageBox::information(this, tr("Recording Metadata"), tr("No metadata snapshot has been captured yet.\nStart a recording to capture one."));
    return;
  }

  QMessageBox box(QMessageBox::Information, tr("Recording Metadata"), m_metadataSnapshotSummary, QMessageBox::Ok, this);
  box.setDetailedText(m_metadataSnapshotDetails);
  box.exec();
}

