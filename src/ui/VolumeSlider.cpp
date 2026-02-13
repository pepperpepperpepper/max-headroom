#include "ui/VolumeSlider.h"

#include <algorithm>

#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

namespace {
constexpr int kNotchValue = 100;
constexpr int kNotchLengthPx = 6;
} // namespace

VolumeSlider::VolumeSlider(Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent)
{
}

void VolumeSlider::paintEvent(QPaintEvent* event)
{
  QSlider::paintEvent(event);

  if (orientation() != Qt::Horizontal) {
    return;
  }
  if (kNotchValue < minimum() || kNotchValue > maximum()) {
    return;
  }

  QStyleOptionSlider opt;
  initStyleOption(&opt);

  const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
  const QRect handleRect = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
  if (!grooveRect.isValid() || !handleRect.isValid()) {
    return;
  }

  const int handleLength = std::max(1, handleRect.width());
  const int grooveSpan = grooveRect.width() - handleLength;
  if (grooveSpan <= 0) {
    return;
  }

  const int handlePos = QStyle::sliderPositionFromValue(minimum(), maximum(), kNotchValue, grooveSpan, opt.upsideDown);
  const int notchX = grooveRect.x() + handlePos + (handleLength / 2);

  int y1 = grooveRect.bottom() + 2;
  int y2 = y1 + kNotchLengthPx;
  if (y2 > rect().bottom() - 1) {
    y2 = grooveRect.top() - 2;
    y1 = y2 - kNotchLengthPx;
  }
  y1 = std::clamp(y1, rect().top() + 1, rect().bottom() - 1);
  y2 = std::clamp(y2, rect().top() + 1, rect().bottom() - 1);
  if (y2 <= y1) {
    return;
  }

  QColor notchColor = palette().color(QPalette::WindowText);
  notchColor.setAlpha(120);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setPen(QPen(notchColor, 1));
  painter.drawLine(QPoint(notchX, y1), QPoint(notchX, y2));
}

