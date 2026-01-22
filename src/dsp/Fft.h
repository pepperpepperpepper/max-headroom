#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace dsp {

class Fft final
{
public:
  static bool isPowerOfTwo(std::size_t n);
  static std::vector<float> hannWindow(std::size_t n);

  // In-place forward FFT (no scaling).
  static void forward(std::vector<std::complex<float>>& data);

  // Convenience for real input; returns complex spectrum of size N.
  static std::vector<std::complex<float>> forwardReal(const std::vector<float>& input);
};

} // namespace dsp
