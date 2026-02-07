#pragma once

#include <QWidget>

class AudioLevelTap;
class QShowEvent;
class QHideEvent;

class LevelMeterWidget final : public QWidget
{
  Q_OBJECT

public:
  enum class Style {
    SimplePeak = 0,
    DetailedPeakRms = 1,
  };

  explicit LevelMeterWidget(QWidget* parent = nullptr);

  void setTap(AudioLevelTap* tap);
  void setStyle(Style style);
  void setActive(bool active);
  bool isActive() const;
  void tick();

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

private:
  static float amplitudeToDb(float amp);
  static float dbToNormalized(float db, float minDb);
  void updateTapEnabled();

  AudioLevelTap* m_tap = nullptr;
  Style m_style = Style::DetailedPeakRms;
  bool m_active = false;
  float m_peakNorm = 0.0f;
  float m_rmsNorm = 0.0f;
  float m_peakHoldNorm = 0.0f;
  int m_peakHoldTicks = 0;
  bool m_clipped = false;
};
