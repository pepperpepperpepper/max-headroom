#pragma once

#include <QWidget>
#include <QPointer>

class QLineEdit;
class QScrollArea;
class QTimer;
class QComboBox;
class QPushButton;
class PipeWireGraph;
class EqManager;
class PipeWireThread;
class LevelMeterWidget;

class MixerPage final : public QWidget
{
  Q_OBJECT

public:
  explicit MixerPage(PipeWireThread* pw, PipeWireGraph* graph, EqManager* eq, QWidget* parent = nullptr);

signals:
  void visualizerTapRequested(QString targetObject, bool captureSink);

public slots:
  void refresh();

private:
  void scheduleRebuild();
  void rebuild();
  void tickMeters();

  PipeWireThread* m_pw = nullptr;
  PipeWireGraph* m_graph = nullptr;
  EqManager* m_eq = nullptr;
  QLineEdit* m_filter = nullptr;
  QComboBox* m_defaultOutput = nullptr;
  QPushButton* m_setDefaultOutput = nullptr;
  QComboBox* m_defaultInput = nullptr;
  QPushButton* m_setDefaultInput = nullptr;
  QScrollArea* m_scroll = nullptr;
  QWidget* m_container = nullptr;
  QTimer* m_rebuildTimer = nullptr;
  QTimer* m_meterTimer = nullptr;
  QList<QPointer<LevelMeterWidget>> m_meters;
};
