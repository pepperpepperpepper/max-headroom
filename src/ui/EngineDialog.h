#pragma once

#include <QDialog>

#include <QString>

class QLabel;
class QComboBox;
class QPushButton;
class QTimer;
class PipeWireGraph;

class EngineDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit EngineDialog(PipeWireGraph* graph, QWidget* parent = nullptr);
  ~EngineDialog() override;

private:
  void rebuildUi();
  void refresh();
  void runAction(const QString& action, const QString& unit);

  void refreshClockUi();
  void applyClockPreset();
  void applyClockOverrides();
  void refreshDiagnosticsUi();
  void refreshMidiBridgeUi();

  PipeWireGraph* m_graph = nullptr;

  QLabel* m_summaryLabel = nullptr;
  QPushButton* m_refreshButton = nullptr;

  struct Row final {
    QString unit;
    QLabel* nameLabel = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* startButton = nullptr;
    QPushButton* stopButton = nullptr;
    QPushButton* restartButton = nullptr;
  };

  QVector<Row> m_rows;

  QLabel* m_clockStatusLabel = nullptr;
  QComboBox* m_clockPresetCombo = nullptr;
  QPushButton* m_clockPresetApply = nullptr;
  QComboBox* m_forceRateCombo = nullptr;
  QComboBox* m_forceQuantumCombo = nullptr;
  QPushButton* m_clockApply = nullptr;
  QPushButton* m_clockReset = nullptr;

  QLabel* m_diagStatusLabel = nullptr;
  QLabel* m_diagDriversLabel = nullptr;
  QTimer* m_diagTimer = nullptr;

  QLabel* m_midiBridgeStatusLabel = nullptr;
  QPushButton* m_midiBridgeEnableButton = nullptr;
  QPushButton* m_midiBridgeDisableButton = nullptr;
};
