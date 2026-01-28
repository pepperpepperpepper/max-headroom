#include "RecorderDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

#include "backend/AudioRecorder.h"
#include "backend/PipeWireGraph.h"

namespace {

QString defaultRecordingPath()
{
  return QDir::home().filePath(QStringLiteral("headroom-recording-{datetime}-{target}.{ext}"));
}

} // namespace

RecorderDialog::RecorderDialog(PipeWireGraph* graph, AudioRecorder* recorder, QWidget* parent)
    : QDialog(parent)
    , m_graph(graph)
    , m_recorder(recorder)
{
  setWindowTitle(tr("Recording"));
  setModal(true);
  resize(640, 360);

  m_targetCombo = new QComboBox(this);

  m_formatCombo = new QComboBox(this);
  m_formatCombo->addItem(tr("WAV (float32)"), AudioRecorder::formatToString(AudioRecorder::Format::Wav));
  m_formatCombo->addItem(tr("FLAC (24-bit)"), AudioRecorder::formatToString(AudioRecorder::Format::Flac));

  m_fileEdit = new QLineEdit(this);
  m_fileEdit->setPlaceholderText(tr("Output path or template (.wav/.flac)"));

  m_durationSpin = new QSpinBox(this);
  m_durationSpin->setRange(0, 24 * 60 * 60);
  m_durationSpin->setSuffix(tr(" s"));
  m_durationSpin->setToolTip(tr("Auto-stop after N seconds. Set 0 to disable."));

  m_browseButton = new QPushButton(tr("Browse…"), this);
  connect(m_browseButton, &QPushButton::clicked, this, &RecorderDialog::browseFile);

  auto* fileRow = new QWidget(this);
  {
    auto* rowLayout = new QHBoxLayout(fileRow);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(m_fileEdit, 1);
    rowLayout->addWidget(m_browseButton);
  }

  m_previewLabel = new QLabel(this);
  m_previewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto* form = new QFormLayout();
  form->addRow(tr("Target:"), m_targetCombo);
  form->addRow(tr("Format:"), m_formatCombo);
  form->addRow(tr("Stop After:"), m_durationSpin);
  form->addRow(tr("Path/Template:"), fileRow);
  form->addRow(tr("Preview:"), m_previewLabel);

  m_stateLabel = new QLabel(this);
  m_formatLabel = new QLabel(this);
  m_bytesLabel = new QLabel(this);
  m_levelsLabel = new QLabel(this);
  m_metadataLabel = new QLabel(this);
  m_metadataButton = new QPushButton(tr("View…"), this);
  connect(m_metadataButton, &QPushButton::clicked, this, &RecorderDialog::showMetadata);
  m_errorLabel = new QLabel(this);
  m_errorLabel->setWordWrap(true);
  m_errorLabel->setStyleSheet(QStringLiteral("color: #c00;"));

  auto* statusBox = new QGroupBox(tr("Status"), this);
  {
    auto* statusLayout = new QFormLayout(statusBox);
    statusLayout->addRow(tr("State:"), m_stateLabel);
    statusLayout->addRow(tr("Format:"), m_formatLabel);
    statusLayout->addRow(tr("Captured:"), m_bytesLabel);
    statusLayout->addRow(tr("Levels:"), m_levelsLabel);

    auto* metaRow = new QWidget(statusBox);
    auto* metaLayout = new QHBoxLayout(metaRow);
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->addWidget(m_metadataLabel, 1);
    metaLayout->addWidget(m_metadataButton);
    statusLayout->addRow(tr("Metadata:"), metaRow);

    statusLayout->addRow(tr("Error:"), m_errorLabel);
  }

  m_startStopButton = new QPushButton(tr("Start"), this);
  connect(m_startStopButton, &QPushButton::clicked, this, &RecorderDialog::startStop);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->addButton(m_startStopButton, QDialogButtonBox::ActionRole);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* root = new QVBoxLayout(this);
  root->addLayout(form);
  root->addWidget(statusBox);
  root->addWidget(buttons);

  QSettings s;
  s.beginGroup(QStringLiteral("recording"));
  const QString rememberedPath = s.value(QStringLiteral("filePath")).toString();
  const QString rememberedFormat = s.value(QStringLiteral("format")).toString();
  const int rememberedDuration = s.value(QStringLiteral("durationSec"), 0).toInt();
  const QString rememberedTarget = s.value(QStringLiteral("targetObject")).toString();
  const bool rememberedCaptureSink = s.value(QStringLiteral("captureSink"), true).toBool();
  s.endGroup();

  m_fileEdit->setText(rememberedPath.isEmpty() ? defaultRecordingPath() : rememberedPath);
  m_durationSpin->setValue(std::max(0, rememberedDuration));

  {
    AudioRecorder::Format f = AudioRecorder::Format::Wav;
    if (const auto parsed = AudioRecorder::formatFromString(rememberedFormat)) {
      f = *parsed;
    } else {
      const QString lp = m_fileEdit->text().trimmed().toLower();
      if (lp.endsWith(QStringLiteral(".flac"))) {
        f = AudioRecorder::Format::Flac;
      }
    }

    for (int i = 0; i < m_formatCombo->count(); ++i) {
      if (m_formatCombo->itemData(i).toString() == AudioRecorder::formatToString(f)) {
        m_formatCombo->setCurrentIndex(i);
        break;
      }
    }
  }

  rebuildTargets();

  // Restore selection if possible.
  for (int i = 0; i < m_targetCombo->count(); ++i) {
    const QVariant v = m_targetCombo->itemData(i);
    if (!v.isValid()) {
      continue;
    }
    const QVariantMap d = v.toMap();
    if (!d.contains(QStringLiteral("targetObject")) || !d.contains(QStringLiteral("captureSink"))) {
      continue;
    }
    if (d.value(QStringLiteral("targetObject")).toString() == rememberedTarget &&
        d.value(QStringLiteral("captureSink")).toBool() == rememberedCaptureSink) {
      m_targetCombo->setCurrentIndex(i);
      break;
    }
  }

  connect(m_targetCombo, &QComboBox::currentIndexChanged, this, [this]() {
    const QVariant v = m_targetCombo->currentData();
    if (!v.isValid()) {
      return;
    }
    const QVariantMap d = v.toMap();
    if (!d.contains(QStringLiteral("targetObject")) || !d.contains(QStringLiteral("captureSink"))) {
      return;
    }

    QSettings s;
    s.beginGroup(QStringLiteral("recording"));
    s.setValue(QStringLiteral("targetObject"), d.value(QStringLiteral("targetObject")).toString());
    s.setValue(QStringLiteral("captureSink"), d.value(QStringLiteral("captureSink")).toBool());
    s.endGroup();

    syncUi();
  });

  connect(m_fileEdit, &QLineEdit::editingFinished, this, [this]() {
    QSettings s;
    s.beginGroup(QStringLiteral("recording"));
    s.setValue(QStringLiteral("filePath"), m_fileEdit->text().trimmed());
    s.endGroup();
  });

  connect(m_fileEdit, &QLineEdit::textChanged, this, [this]() { syncUi(); });

  connect(m_formatCombo, &QComboBox::currentIndexChanged, this, [this]() {
    const auto f = AudioRecorder::formatFromString(m_formatCombo->currentData().toString()).value_or(AudioRecorder::Format::Wav);

    QSettings s;
    s.beginGroup(QStringLiteral("recording"));
    s.setValue(QStringLiteral("format"), AudioRecorder::formatToString(f));
    s.endGroup();

    const QString current = m_fileEdit->text().trimmed();
    if (!current.isEmpty() && !current.contains(QLatin1Char('{'))) {
      const QString adjusted = AudioRecorder::ensureFileExtension(current, f);
      if (adjusted != current) {
        m_fileEdit->setText(adjusted);
      }
    }

    syncUi();
  });

  connect(m_durationSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
    QSettings s;
    s.beginGroup(QStringLiteral("recording"));
    s.setValue(QStringLiteral("durationSec"), std::max(0, v));
    s.endGroup();
  });

  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &RecorderDialog::rebuildTargets);
  }

  if (m_recorder) {
    connect(m_recorder, &AudioRecorder::recordingChanged, this, [this]() { syncUi(); });
    connect(m_recorder, &AudioRecorder::errorOccurred, this, [this](const QString&) { syncUi(); });
  }

  m_statusTimer = new QTimer(this);
  m_statusTimer->setInterval(200);
  connect(m_statusTimer, &QTimer::timeout, this, &RecorderDialog::syncUi);
  m_statusTimer->start();

  syncUi();
}

RecorderDialog::~RecorderDialog() = default;
