#pragma once

#include <QWidget>

class AudioLevelTap;

class LevelMeterWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit LevelMeterWidget(QWidget* parent = nullptr);

  void setTap(AudioLevelTap* tap);
  void tick();

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  static float amplitudeToDb(float amp);
  static float dbToNormalized(float db, float minDb);

  AudioLevelTap* m_tap = nullptr;
  float m_peakNorm = 0.0f;
  float m_rmsNorm = 0.0f;
  float m_peakHoldNorm = 0.0f;
  int m_peakHoldTicks = 0;
  bool m_clipped = false;
};

