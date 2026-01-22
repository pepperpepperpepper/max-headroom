#pragma once

#include <QDialog>

class QListWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class PipeWireGraph;

class SettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit SettingsDialog(PipeWireGraph* graph, QWidget* parent = nullptr);

signals:
  void sinksOrderChanged();
  void layoutSettingsChanged();
  void visualizerSettingsChanged();

private:
  void loadGeneral();
  void loadVisualizer();
  void loadSinksOrder();
  void resetSinksOrder();
  void resetPatchbayLayout();
  void accept() override;

  QStringList currentSinksOrder() const;
  QStringList defaultSinksOrder() const;

  PipeWireGraph* m_graph = nullptr;
  QListWidget* m_sinksList = nullptr;
  QCheckBox* m_layoutEditMode = nullptr;
  QComboBox* m_vizRefresh = nullptr;
  QComboBox* m_vizFftSize = nullptr;
  QSlider* m_vizSmoothing = nullptr;
  QLabel* m_vizSmoothingLabel = nullptr;
  QComboBox* m_vizWaveHistory = nullptr;
  QComboBox* m_vizSpecHistory = nullptr;
  bool m_resetLayoutRequested = false;
};
