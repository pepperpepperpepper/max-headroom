#pragma once

#include <algorithm>
#include <cmath>

namespace headroom::volume {
// Match PulseAudio's recommended UI maximum (PA_VOLUME_UI_MAX, ~+11 dB) as used by pavucontrol.
// PulseAudio volumes are cubic; convert UI percent <-> linear amplitude.
inline constexpr int kUiMaxPercent = 153;
inline constexpr float kUiMaxLinear = 3.548133f; // pa_sw_volume_to_linear(PA_VOLUME_UI_MAX)

inline float uiPercentToLinear(int percent)
{
  if (percent >= kUiMaxPercent) {
    return kUiMaxLinear;
  }

  const float p = std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f);
  return p * p * p;
}

inline int linearToUiPercent(float linear)
{
  if (!(linear > 0.0f)) { // handles 0, negative, and NaN
    return 0;
  }
  if (linear >= kUiMaxLinear) {
    return kUiMaxPercent;
  }
  const double p = std::cbrt(std::max(0.0, static_cast<double>(linear)));
  return static_cast<int>(std::lround(p * 100.0));
}
} // namespace headroom::volume
