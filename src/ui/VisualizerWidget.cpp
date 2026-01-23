#include "VisualizerWidget.h"

#include "backend/AudioTap.h"
#include "settings/VisualizerSettings.h"

#include <algorithm>
#include <QPainter>
#include <QTimer>
#include <QtGlobal>

#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;
} // namespace

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
  const bool demo = qEnvironmentVariableIsSet("HEADROOM_DEMO_VISUALIZER");
  float peak = m_tap->peak();
  float rms = m_tap->rms();
  if (demo && peak <= 0.0001f && rms <= 0.0001f) {
    peak = 0.742f;
    rms = 0.311f;
  }
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
  if (demo && (mins.isEmpty() || maxs.isEmpty())) {
    mins.resize(columns);
    maxs.resize(columns);
    for (int x = 0; x < columns; ++x) {
      const float t = static_cast<float>(x) / static_cast<float>(std::max(1, columns - 1));
      const float phase = t * 2.0f * kPi;
      const float carrier = std::sin(phase * 3.0f) * 0.65f;
      const float mod = std::sin(phase * 0.35f) * 0.18f;
      const float spread = 0.12f + 0.06f * (0.5f + 0.5f * std::sin(phase * 1.6f));
      const float v = carrier + mod;
      mins[x] = std::clamp(v - spread, -1.0f, 1.0f);
      maxs[x] = std::clamp(v + spread, -1.0f, 1.0f);
    }
  }

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
  QVector<float> spectrum = m_tap->spectrum();
  if (demo) {
    if (spectrum.isEmpty()) {
      spectrum.resize(256);
    }
    const int bins = spectrum.size();
    for (int i = 0; i < bins; ++i) {
      const float x = static_cast<float>(i) / static_cast<float>(std::max(1, bins - 1));
      // A couple of smooth peaks + a gentle noise floor.
      const float p1 = std::exp(-0.5f * std::pow((x - 0.10f) / 0.03f, 2.0f));
      const float p2 = std::exp(-0.5f * std::pow((x - 0.23f) / 0.05f, 2.0f));
      const float p3 = std::exp(-0.5f * std::pow((x - 0.58f) / 0.08f, 2.0f));
      const float floor = 0.07f * (0.5f + 0.5f * std::sin(x * 18.0f));
      spectrum[i] = std::clamp(0.75f * p1 + 0.55f * p2 + 0.40f * p3 + floor, 0.0f, 1.0f);
    }
  }
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
  auto spec = m_tap->spectrogram();
  if (spec.columns > 0 && spec.bins > 0 && spec.pixels.size() == spec.columns * spec.bins) {
    QImage img(spec.columns, spec.bins, QImage::Format_ARGB32);
    if (demo) {
      for (int x = 0; x < spec.columns; ++x) {
        const float xf = static_cast<float>(x) / static_cast<float>(std::max(1, spec.columns - 1));
        for (int y = 0; y < spec.bins; ++y) {
          const float yf = static_cast<float>(y) / static_cast<float>(std::max(1, spec.bins - 1));
          const float ridge = std::exp(-0.5f * std::pow((yf - (0.15f + 0.55f * xf)) / 0.06f, 2.0f));
          const float ridge2 = std::exp(-0.5f * std::pow((yf - (0.55f - 0.35f * xf)) / 0.08f, 2.0f));
          const float v = std::clamp(0.12f + 0.78f * ridge + 0.52f * ridge2, 0.0f, 1.0f);
          img.setPixel(x, y, QColor::fromHsvF(0.62 - 0.62 * v, 0.9, 0.95).rgba());
        }
      }
    } else {
      for (int x = 0; x < spec.columns; ++x) {
        const int srcCol = (spec.writeColumn + x) % spec.columns; // oldest -> newest
        for (int y = 0; y < spec.bins; ++y) {
          const int srcRow = spec.bins - 1 - y; // low at bottom
          const int srcIndex = srcCol * spec.bins + srcRow;
          img.setPixel(x, y, spec.pixels[srcIndex]);
        }
      }
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(spectrogramRect.adjusted(2, 2, -2, -2), img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  }
}
