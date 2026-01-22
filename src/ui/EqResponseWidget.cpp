#include "EqResponseWidget.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <complex>

namespace {
constexpr double kPi = 3.14159265358979323846;

double clamp(double v, double lo, double hi)
{
  return std::max(lo, std::min(hi, v));
}

struct Biquad final {
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;
};

double dbToLin(double db)
{
  return std::pow(10.0, db / 20.0);
}

Biquad makeBiquad(EqBandType type, double sampleRate, double freqHz, double q, double gainDb)
{
  Biquad c;
  const double sr = std::max(1.0, sampleRate);
  const double f0 = clamp(freqHz, 1.0, sr * 0.49);
  const double w0 = 2.0 * kPi * f0 / sr;
  const double cs = std::cos(w0);
  const double sn = std::sin(w0);
  const double Q = clamp(q, 0.05, 24.0);
  const double alpha = sn / (2.0 * Q);

  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a0 = 1.0;
  double a1 = 0.0;
  double a2 = 0.0;

  switch (type) {
    case EqBandType::Peaking: {
      const double A = std::pow(10.0, gainDb / 40.0);
      b0 = 1.0 + alpha * A;
      b1 = -2.0 * cs;
      b2 = 1.0 - alpha * A;
      a0 = 1.0 + alpha / A;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha / A;
      break;
    }
    case EqBandType::LowPass: {
      b0 = (1.0 - cs) / 2.0;
      b1 = 1.0 - cs;
      b2 = (1.0 - cs) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::HighPass: {
      b0 = (1.0 + cs) / 2.0;
      b1 = -(1.0 + cs);
      b2 = (1.0 + cs) / 2.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::Notch: {
      b0 = 1.0;
      b1 = -2.0 * cs;
      b2 = 1.0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::BandPass: {
      b0 = alpha;
      b1 = 0.0;
      b2 = -alpha;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::LowShelf: {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double S = clamp(q, 0.05, 24.0);
      const double alphaS = sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
      const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alphaS;

      b0 = A * ((A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha);
      b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
      b2 = A * ((A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha);
      a0 = (A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha;
      a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
      a2 = (A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha;
      break;
    }
    case EqBandType::HighShelf: {
      const double A = std::pow(10.0, gainDb / 40.0);
      const double S = clamp(q, 0.05, 24.0);
      const double alphaS = sn / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
      const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alphaS;

      b0 = A * ((A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha);
      b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
      b2 = A * ((A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha);
      a0 = (A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha;
      a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
      a2 = (A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha;
      break;
    }
  }

  if (a0 == 0.0) {
    return c;
  }

  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

double biquadMagnitudeAtHz(const Biquad& c, double sampleRate, double freqHz)
{
  const double sr = std::max(1.0, sampleRate);
  const double f = clamp(freqHz, 1.0, sr * 0.49);
  const double w = 2.0 * kPi * f / sr;

  const std::complex<double> z1 = std::polar(1.0, -w);
  const std::complex<double> z2 = std::polar(1.0, -2.0 * w);

  const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
  const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
  const std::complex<double> h = (den == std::complex<double>(0.0, 0.0)) ? std::complex<double>(0.0, 0.0) : (num / den);
  return std::abs(h);
}

double responseDbAtHz(const EqPreset& preset, double sampleRate, double freqHz)
{
  if (!preset.enabled) {
    return 0.0;
  }

  double mag = dbToLin(preset.preampDb);
  for (const auto& band : preset.bands) {
    if (!band.enabled) {
      continue;
    }
    const Biquad b = makeBiquad(band.type, sampleRate, band.freqHz, band.q, band.gainDb);
    mag *= biquadMagnitudeAtHz(b, sampleRate, freqHz);
  }

  mag = std::max(1.0e-9, mag);
  return 20.0 * std::log10(mag);
}

double freqForX(double x01)
{
  constexpr double fMin = 20.0;
  constexpr double fMax = 20000.0;
  const double t = clamp(x01, 0.0, 1.0);
  return fMin * std::pow(fMax / fMin, t);
}

int yForDb(double db, const QRect& plot, double minDb, double maxDb)
{
  const double t = (clamp(db, minDb, maxDb) - minDb) / (maxDb - minDb);
  return plot.bottom() - static_cast<int>(std::lround(t * static_cast<double>(plot.height())));
}
} // namespace

EqResponseWidget::EqResponseWidget(QWidget* parent)
    : QWidget(parent)
{
  setMinimumHeight(180);
}

void EqResponseWidget::setPreset(const EqPreset& preset)
{
  m_preset = preset;
  update();
}

void EqResponseWidget::paintEvent(QPaintEvent* /*event*/)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  p.fillRect(rect(), QColor(16, 18, 24));

  const QRect outer = rect().adjusted(10, 10, -10, -10);
  p.setPen(QPen(QColor(40, 46, 60), 1));
  p.setBrush(QColor(12, 14, 18));
  p.drawRoundedRect(outer, 10, 10);

  const QRect plot = outer.adjusted(44, 10, -10, -26);
  const double minDb = -24.0;
  const double maxDb = 24.0;

  // Grid.
  p.setPen(QPen(QColor(32, 36, 48), 1));
  for (double db : {minDb, -12.0, 0.0, 12.0, maxDb}) {
    const int y = yForDb(db, plot, minDb, maxDb);
    p.drawLine(plot.left(), y, plot.right(), y);
    p.setPen(QColor(155, 165, 185));
    p.drawText(QRect(outer.left() + 8, y - 8, 34, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0));
    p.setPen(QPen(QColor(32, 36, 48), 1));
  }

  const QVector<double> xTicks = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  for (double f : xTicks) {
    const double t = std::log(f / 20.0) / std::log(20000.0 / 20.0);
    const int x = plot.left() + static_cast<int>(std::lround(t * static_cast<double>(plot.width())));
    p.drawLine(x, plot.top(), x, plot.bottom());

    p.setPen(QColor(155, 165, 185));
    const QString label = (f >= 1000.0) ? QStringLiteral("%1k").arg(QString::number(f / 1000.0, 'f', (f == 1000.0 || f == 2000.0 || f == 5000.0) ? 0 : 0))
                                        : QString::number(static_cast<int>(f));
    p.drawText(QRect(x - 22, plot.bottom() + 6, 44, 16), Qt::AlignHCenter | Qt::AlignTop, label);
    p.setPen(QPen(QColor(32, 36, 48), 1));
  }

  // Response curve.
  QPainterPath path;
  const int samples = std::max(2, plot.width());
  for (int i = 0; i < samples; ++i) {
    const double x01 = static_cast<double>(i) / static_cast<double>(samples - 1);
    const double f = freqForX(x01);
    const double db = responseDbAtHz(m_preset, m_sampleRate, f);
    const int x = plot.left() + i;
    const int y = yForDb(db, plot, minDb, maxDb);
    if (i == 0) {
      path.moveTo(x, y);
    } else {
      path.lineTo(x, y);
    }
  }

  p.setPen(QPen(QColor(99, 102, 241), 2));
  p.drawPath(path);

  // Baseline at 0 dB.
  p.setPen(QPen(QColor(80, 88, 110), 1, Qt::DashLine));
  const int y0 = yForDb(0.0, plot, minDb, maxDb);
  p.drawLine(plot.left(), y0, plot.right(), y0);
}
