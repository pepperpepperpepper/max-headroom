#pragma once

#include <QDialog>
#include <QString>

class AudioRecorder;
class PipeWireGraph;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class RecorderDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit RecorderDialog(PipeWireGraph* graph, AudioRecorder* recorder, QWidget* parent = nullptr);
  ~RecorderDialog() override;

private:
  void rebuildTargets();
  void syncUi();
  void browseFile();
  void startStop();
  void showMetadata();

  PipeWireGraph* m_graph = nullptr;
  AudioRecorder* m_recorder = nullptr;

  QComboBox* m_targetCombo = nullptr;
  QComboBox* m_formatCombo = nullptr;
  QLineEdit* m_fileEdit = nullptr;
  QSpinBox* m_durationSpin = nullptr;
  QPushButton* m_browseButton = nullptr;
  QPushButton* m_startStopButton = nullptr;
  QLabel* m_stateLabel = nullptr;
  QLabel* m_formatLabel = nullptr;
  QLabel* m_bytesLabel = nullptr;
  QLabel* m_levelsLabel = nullptr;
  QLabel* m_metadataLabel = nullptr;
  QPushButton* m_metadataButton = nullptr;
  QLabel* m_previewLabel = nullptr;
  QLabel* m_errorLabel = nullptr;
  QTimer* m_statusTimer = nullptr;

  bool m_hasMetadataSnapshot = false;
  QString m_metadataSnapshotSummary;
  QString m_metadataSnapshotDetails;
};
