#pragma once

#include <QWidget>

class QLabel;
class QComboBox;
class AudioTap;
class PipeWireGraph;
class VisualizerWidget;
struct VisualizerSettings;

class VisualizerPage final : public QWidget
{
  Q_OBJECT

public:
  explicit VisualizerPage(PipeWireGraph* graph, AudioTap* tap, QWidget* parent = nullptr);

public slots:
  void setTapTarget(const QString& targetObject, bool captureSink);
  void applySettings(const VisualizerSettings& settings);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  void repopulateSources();

  PipeWireGraph* m_graph = nullptr;
  AudioTap* m_tap = nullptr;
  VisualizerWidget* m_widget = nullptr;
  QComboBox* m_sources = nullptr;
  QLabel* m_state = nullptr;
  bool m_userHasChosenTarget = false;
  QString m_pendingTargetObject;
  bool m_pendingCaptureSink = false;
  bool m_hasPendingTarget = false;
};
