#pragma once

#include "settings/SettingsKeys.h"

#include <QSettings>

#include <algorithm>
#include <cmath>

struct VisualizerSettings final {
  int refreshIntervalMs = 33;           // ~30 FPS
  int fftSize = 1024;                   // power of two
  double spectrumSmoothing = 0.92;      // 0..1, higher = more smoothing
  double waveformHistorySeconds = 0.5;  // seconds
  double spectrogramHistorySeconds = 2; // seconds
};

namespace VisualizerSettingsStore {
inline VisualizerSettings defaults()
{
  return VisualizerSettings{};
}

inline VisualizerSettings load(QSettings& s)
{
  VisualizerSettings out = defaults();

  out.refreshIntervalMs = s.value(SettingsKeys::visualizerRefreshIntervalMs(), out.refreshIntervalMs).toInt();
  out.refreshIntervalMs = std::clamp(out.refreshIntervalMs, 8, 250);

  out.fftSize = s.value(SettingsKeys::visualizerFftSize(), out.fftSize).toInt();
  out.fftSize = std::clamp(out.fftSize, 256, 16384);

  out.spectrumSmoothing = s.value(SettingsKeys::visualizerSpectrumSmoothing(), out.spectrumSmoothing).toDouble();
  out.spectrumSmoothing = std::clamp(out.spectrumSmoothing, 0.0, 0.999);

  out.waveformHistorySeconds = s.value(SettingsKeys::visualizerWaveformHistorySeconds(), out.waveformHistorySeconds).toDouble();
  out.waveformHistorySeconds = std::clamp(out.waveformHistorySeconds, 0.05, 30.0);

  out.spectrogramHistorySeconds = s.value(SettingsKeys::visualizerSpectrogramHistorySeconds(), out.spectrogramHistorySeconds).toDouble();
  out.spectrogramHistorySeconds = std::clamp(out.spectrogramHistorySeconds, 0.25, 120.0);

  return out;
}

inline bool approxEqual(const VisualizerSettings& a, const VisualizerSettings& b)
{
  auto eqD = [](double x, double y) { return std::abs(x - y) < 1.0e-6; };
  return a.refreshIntervalMs == b.refreshIntervalMs && a.fftSize == b.fftSize && eqD(a.spectrumSmoothing, b.spectrumSmoothing)
      && eqD(a.waveformHistorySeconds, b.waveformHistorySeconds) && eqD(a.spectrogramHistorySeconds, b.spectrogramHistorySeconds);
}

inline void save(QSettings& s, const VisualizerSettings& v)
{
  const VisualizerSettings d = defaults();

  auto setOrRemove = [&](const QString& key, const QVariant& value, const QVariant& def) {
    if (value == def) {
      s.remove(key);
    } else {
      s.setValue(key, value);
    }
  };

  setOrRemove(SettingsKeys::visualizerRefreshIntervalMs(), v.refreshIntervalMs, d.refreshIntervalMs);
  setOrRemove(SettingsKeys::visualizerFftSize(), v.fftSize, d.fftSize);
  setOrRemove(SettingsKeys::visualizerSpectrumSmoothing(), v.spectrumSmoothing, d.spectrumSmoothing);
  setOrRemove(SettingsKeys::visualizerWaveformHistorySeconds(), v.waveformHistorySeconds, d.waveformHistorySeconds);
  setOrRemove(SettingsKeys::visualizerSpectrogramHistorySeconds(), v.spectrogramHistorySeconds, d.spectrogramHistorySeconds);
}
} // namespace VisualizerSettingsStore

