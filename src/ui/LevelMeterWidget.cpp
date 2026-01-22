#include "LevelMeterWidget.h"

#include "backend/AudioLevelTap.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMinDb = -60.0f;
}

LevelMeterWidget::LevelMeterWidget(QWidget* parent)
    : QWidget(parent)
{
  setAutoFillBackground(false);
  setFixedHeight(14);
  setMinimumWidth(110);
  setMaximumWidth(140);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void LevelMeterWidget::setTap(AudioLevelTap* tap)
{
  m_tap = tap;
}

float LevelMeterWidget::amplitudeToDb(float amp)
{
  if (amp <= 1.0e-9f) {
    return -120.0f;
  }
  return 20.0f * std::log10(amp);
}

float LevelMeterWidget::dbToNormalized(float db, float minDb)
{
  const float v = (db - minDb) / (0.0f - minDb);
  return std::clamp(v, 0.0f, 1.0f);
}

void LevelMeterWidget::tick()
{
  if (!m_tap) {
    m_peakNorm = 0.0f;
    m_rmsNorm = 0.0f;
    m_peakHoldNorm = 0.0f;
    m_peakHoldTicks = 0;
    m_clipped = false;
    update();
    return;
  }

  const float peakDb = amplitudeToDb(m_tap->peak());
  const float rmsDb = amplitudeToDb(m_tap->rms());
  const float peakTarget = dbToNormalized(peakDb, kMinDb);
  const float rmsTarget = dbToNormalized(rmsDb, kMinDb);

  // Attack fast, release slow.
  auto smooth = [](float current, float target) {
    const float attack = 0.55f;
    const float release = 0.08f;
    const float k = (target > current) ? attack : release;
    return current + (target - current) * k;
  };

  m_peakNorm = smooth(m_peakNorm, peakTarget);
  m_rmsNorm = smooth(m_rmsNorm, rmsTarget);

  // Simple peak hold.
  if (peakTarget >= m_peakHoldNorm) {
    m_peakHoldNorm = peakTarget;
    m_peakHoldTicks = 10; // ~330ms at 33ms tick
  } else if (m_peakHoldTicks > 0) {
    --m_peakHoldTicks;
  } else {
    m_peakHoldNorm = std::max(m_peakNorm, m_peakHoldNorm * 0.94f);
  }

  m_clipped = m_tap->clippedRecently(900);
  update();
}

void LevelMeterWidget::paintEvent(QPaintEvent* /*event*/)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  QRect r = rect().adjusted(0, 0, -1, -1);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(18, 22, 30));
  p.drawRoundedRect(r, 6, 6);

  QRect bar = r.adjusted(2, 2, -2, -2);
  const int w = std::max(0, static_cast<int>(std::lround(bar.width() * m_rmsNorm)));

  // RMS fill.
  QLinearGradient g(bar.topLeft(), bar.topRight());
  g.setColorAt(0.0, QColor(34, 197, 94));
  g.setColorAt(0.7, QColor(234, 179, 8));
  g.setColorAt(1.0, QColor(239, 68, 68));

  p.setBrush(g);
  p.drawRoundedRect(QRect(bar.left(), bar.top(), w, bar.height()), 4, 4);

  // Peak hold line.
  const int px = bar.left() + static_cast<int>(std::lround(bar.width() * m_peakHoldNorm));
  p.setPen(QPen(QColor(226, 232, 240), 1));
  p.drawLine(px, bar.top(), px, bar.bottom());

  // Clip indicator.
  if (m_clipped) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(239, 68, 68));
    QRect c(bar.right() - 6, bar.top(), 6, bar.height());
    p.drawRoundedRect(c, 3, 3);
  }

  // Border.
  p.setPen(QPen(QColor(51, 65, 85), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(r, 6, 6);
}

