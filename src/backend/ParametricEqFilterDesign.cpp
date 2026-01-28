#include "ParametricEqFilter.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

double ParametricEqFilter::clamp(double v, double lo, double hi)
{
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

ParametricEqFilter::Biquad ParametricEqFilter::makeBiquad(EqBandType type, double sampleRate, double freqHz, double q, double gainDb)
{
  Biquad c;
  if (sampleRate <= 0.0) {
    return c;
  }

  const double fs = sampleRate;
  const double f0 = clamp(freqHz, 1.0, fs * 0.49);
  const double Q = clamp(q, 0.05, 24.0);

  const double w0 = 2.0 * kPi * (f0 / fs);
  const double cs = std::cos(w0);
  const double sn = std::sin(w0);
  const double alpha = sn / (2.0 * Q);

  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

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
      b0 = (1.0 - cs) * 0.5;
      b1 = 1.0 - cs;
      b2 = (1.0 - cs) * 0.5;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cs;
      a2 = 1.0 - alpha;
      break;
    }
    case EqBandType::HighPass: {
      b0 = (1.0 + cs) * 0.5;
      b1 = -(1.0 + cs);
      b2 = (1.0 + cs) * 0.5;
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

void ParametricEqFilter::rebuildCoefficientsLocked()
{
  const int channels = std::max(1, m_totalChannels);
  const int bands = m_preset.bands.size();

  m_biquads.resize(channels);
  m_biquadState.resize(channels);
  for (int ch = 0; ch < channels; ++ch) {
    m_biquads[ch].resize(bands);
    m_biquadState[ch].resize(bands);
  }

  for (int i = 0; i < bands; ++i) {
    const EqBand band = m_preset.bands[i];
    const Biquad biq = makeBiquad(band.type, m_sampleRate, band.freqHz, band.q, band.gainDb);
    for (int ch = 0; ch < channels; ++ch) {
      m_biquads[ch][i] = biq;
      m_biquadState[ch][i] = BiquadState{};
    }
  }
}

