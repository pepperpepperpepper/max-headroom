#pragma once

#include <QWidget>

class AudioTap;
class QTimer;
struct VisualizerSettings;

class VisualizerWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit VisualizerWidget(AudioTap* tap, QWidget* parent = nullptr);

public slots:
  void applySettings(const VisualizerSettings& settings);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

private:
  AudioTap* m_tap = nullptr;
  QTimer* m_timer = nullptr;
  int m_refreshIntervalMs = 33;
  double m_waveformHistorySeconds = 0.5;
};
