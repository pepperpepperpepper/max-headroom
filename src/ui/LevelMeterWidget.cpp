#include "LevelMeterWidget.h"

#include "backend/AudioLevelTap.h"
#include "ui/WindowVisibility.h"

#include <QHideEvent>
#include <QPainter>
#include <QShowEvent>

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
  if (m_tap == tap) {
    return;
  }
  if (m_tap) {
    m_tap->setEnabled(false);
  }
  m_tap = tap;
  updateTapEnabled();
}

void LevelMeterWidget::setStyle(Style style)
{
  if (m_style == style) {
    return;
  }
  m_style = style;
  updateTapEnabled();
  update();
}

void LevelMeterWidget::setActive(bool active)
{
  if (m_active == active) {
    return;
  }

  m_active = active;
  updateTapEnabled();

  if (!m_active) {
    m_peakNorm = 0.0f;
    m_rmsNorm = 0.0f;
    m_peakHoldNorm = 0.0f;
    m_peakHoldTicks = 0;
    m_clipped = false;
    update();
  }
}

bool LevelMeterWidget::isActive() const
{
  return m_active;
}

void LevelMeterWidget::updateTapEnabled()
{
  if (m_tap) {
    m_tap->setComputeMode(
        m_style == Style::SimplePeak ? AudioLevelTap::ComputeMode::PeakOnly : AudioLevelTap::ComputeMode::PeakAndRms);
    m_tap->setEnabled(m_active && WindowVisibility::isActive(window()));
  }
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
  if (!m_active || !m_tap) {
    return;
  }

  const float peakDb = amplitudeToDb(m_tap->peak());
  const float peakTarget = dbToNormalized(peakDb, kMinDb);
  const float rmsTarget = [&]() {
    if (m_style == Style::DetailedPeakRms) {
      const float rmsDb = amplitudeToDb(m_tap->rms());
      return dbToNormalized(rmsDb, kMinDb);
    }
    return peakTarget;
  }();

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

void LevelMeterWidget::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  updateTapEnabled();
}

void LevelMeterWidget::hideEvent(QHideEvent* event)
{
  QWidget::hideEvent(event);
  updateTapEnabled();
}

void LevelMeterWidget::paintEvent(QPaintEvent* /*event*/)
{
  QPainter p(this);
  const bool detailed = (m_style == Style::DetailedPeakRms);
  p.setRenderHint(QPainter::Antialiasing, detailed);

  QRect r = rect().adjusted(0, 0, -1, -1);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(18, 22, 30));
  if (detailed) {
    p.drawRoundedRect(r, 6, 6);
  } else {
    p.drawRect(r);
  }

  QRect bar = r.adjusted(2, 2, -2, -2);
  const float fillNorm = detailed ? m_rmsNorm : m_peakNorm;
  const int w = std::max(0, static_cast<int>(std::lround(bar.width() * fillNorm)));

  if (detailed) {
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
  } else {
    const QColor fill = [&]() {
      if (fillNorm >= 0.90f) {
        return QColor(239, 68, 68);
      }
      if (fillNorm >= 0.70f) {
        return QColor(234, 179, 8);
      }
      return QColor(34, 197, 94);
    }();

    p.setBrush(fill);
    p.drawRect(QRect(bar.left(), bar.top(), w, bar.height()));
  }

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
  if (detailed) {
    p.drawRoundedRect(r, 6, 6);
  } else {
    p.drawRect(r);
  }
}
