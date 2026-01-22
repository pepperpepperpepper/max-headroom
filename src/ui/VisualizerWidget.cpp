#include "VisualizerWidget.h"

#include "backend/AudioTap.h"
#include "settings/VisualizerSettings.h"

#include <algorithm>
#include <QPainter>
#include <QTimer>

#include <cmath>

VisualizerWidget::VisualizerWidget(AudioTap* tap, QWidget* parent)
    : QWidget(parent)
    , m_tap(tap)
{
  setAutoFillBackground(false);
  setMinimumHeight(280);

  m_timer = new QTimer(this);
  connect(m_timer, &QTimer::timeout, this, QOverload<>::of(&VisualizerWidget::update));

  QSettings s;
  applySettings(VisualizerSettingsStore::load(s));
}

void VisualizerWidget::applySettings(const VisualizerSettings& settings)
{
  m_refreshIntervalMs = std::clamp(settings.refreshIntervalMs, 8, 250);
  m_waveformHistorySeconds = std::clamp(settings.waveformHistorySeconds, 0.05, 30.0);

  if (m_timer) {
    m_timer->setInterval(m_refreshIntervalMs);
    if (!m_timer->isActive()) {
      m_timer->start();
    }
  }

  update();
}

static void drawPanel(QPainter& p, const QRect& r, const QColor& top, const QColor& bottom)
{
  QLinearGradient g(r.topLeft(), r.bottomLeft());
  g.setColorAt(0.0, top);
  g.setColorAt(1.0, bottom);
  p.setPen(Qt::NoPen);
  p.setBrush(g);
  p.drawRoundedRect(r, 12, 12);
}

void VisualizerWidget::paintEvent(QPaintEvent* /*event*/)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(rect(), QColor(12, 14, 18));

  const QRect outer = rect().adjusted(12, 12, -12, -12);
  drawPanel(p, outer, QColor(20, 24, 32), QColor(10, 12, 18));

  const QRect inner = outer.adjusted(12, 12, -12, -12);

  QRect waveformRect = inner;
  waveformRect.setHeight(inner.height() * 3 / 10);

  QRect spectrumRect = inner;
  spectrumRect.setTop(waveformRect.bottom() + 10);
  spectrumRect.setHeight(inner.height() * 2 / 10);

  QRect spectrogramRect = inner;
  spectrogramRect.setTop(spectrumRect.bottom() + 10);
  spectrogramRect.setBottom(inner.bottom());

  p.setPen(QPen(QColor(40, 46, 60), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(waveformRect, 10, 10);
  p.drawRoundedRect(spectrumRect, 10, 10);
  p.drawRoundedRect(spectrogramRect, 10, 10);

  if (!m_tap) {
    p.setPen(QColor(180, 190, 210));
    p.drawText(inner, Qt::AlignCenter, tr("No audio tap"));
    return;
  }

  // Header (meters).
  const float peak = m_tap->peak();
  const float rms = m_tap->rms();
  p.setPen(QColor(185, 195, 215));
  p.drawText(QRect(inner.left(), inner.top() - 4, inner.width(), 16),
             Qt::AlignLeft | Qt::AlignVCenter,
             tr("Peak: %1  RMS: %2").arg(QString::number(peak, 'f', 3), QString::number(rms, 'f', 3)));

  // Waveform.
  QVector<float> mins;
  QVector<float> maxs;
  const int columns = std::max(1, waveformRect.width() - 4);
  const uint32_t sr = m_tap->sampleRate();
  const int windowSamples = std::max(1, static_cast<int>(std::llround(m_waveformHistorySeconds * static_cast<double>(sr))));
  m_tap->waveformMinMax(columns, windowSamples, mins, maxs);

  const float midY = waveformRect.center().y();
  const float amp = (waveformRect.height() * 0.45f);
  p.setRenderHint(QPainter::Antialiasing, false);
  p.setPen(QPen(QColor(99, 102, 241), 1));
  for (int x = 0; x < columns && x < mins.size() && x < maxs.size(); ++x) {
    const float minV = std::clamp(mins[x], -1.0f, 1.0f);
    const float maxV = std::clamp(maxs[x], -1.0f, 1.0f);
    const int px = waveformRect.left() + 2 + x;
    const int y1 = static_cast<int>(midY - maxV * amp);
    const int y2 = static_cast<int>(midY - minV * amp);
    p.drawLine(px, y1, px, y2);
  }
  p.setRenderHint(QPainter::Antialiasing, true);

  // Spectrum.
  const auto spectrum = m_tap->spectrum();
  if (!spectrum.isEmpty()) {
    p.setPen(Qt::NoPen);
    const int bins = spectrum.size();
    const float barW = static_cast<float>(spectrumRect.width()) / static_cast<float>(bins);
    for (int i = 0; i < bins; ++i) {
      const float v = std::clamp(spectrum[i], 0.0f, 1.0f);
      const int h = static_cast<int>(v * (spectrumRect.height() - 6));
      const QRect bar(spectrumRect.left() + static_cast<int>(i * barW),
                      spectrumRect.bottom() - 3 - h,
                      std::max(1, static_cast<int>(barW)),
                      h);
      const QColor c = QColor::fromHsvF(0.58 - 0.58 * v, 0.85, 0.95);
      p.setBrush(c);
      p.drawRect(bar);
    }
  }

  // Spectrogram.
  const auto spec = m_tap->spectrogram();
  if (spec.columns > 0 && spec.bins > 0 && spec.pixels.size() == spec.columns * spec.bins) {
    QImage img(spec.columns, spec.bins, QImage::Format_ARGB32);
    for (int x = 0; x < spec.columns; ++x) {
      const int srcCol = (spec.writeColumn + x) % spec.columns; // oldest -> newest
      for (int y = 0; y < spec.bins; ++y) {
        const int srcRow = spec.bins - 1 - y; // low at bottom
        const int srcIndex = srcCol * spec.bins + srcRow;
        img.setPixel(x, y, spec.pixels[srcIndex]);
      }
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(spectrogramRect.adjusted(2, 2, -2, -2), img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  }
}
