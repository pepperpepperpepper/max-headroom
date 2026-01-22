#include "Fft.h"

#include <cmath>

namespace dsp {

bool Fft::isPowerOfTwo(std::size_t n)
{
  return n != 0 && (n & (n - 1)) == 0;
}

std::vector<float> Fft::hannWindow(std::size_t n)
{
  std::vector<float> w(n, 0.0f);
  if (n == 0) {
    return w;
  }

  constexpr float twoPi = 6.2831853071795864769f;
  for (std::size_t i = 0; i < n; ++i) {
    w[i] = 0.5f * (1.0f - std::cos(twoPi * static_cast<float>(i) / static_cast<float>(n - 1)));
  }
  return w;
}

static std::size_t reverseBits(std::size_t x, int bits)
{
  std::size_t y = 0;
  for (int i = 0; i < bits; ++i) {
    y = (y << 1U) | (x & 1U);
    x >>= 1U;
  }
  return y;
}

void Fft::forward(std::vector<std::complex<float>>& data)
{
  const std::size_t n = data.size();
  if (!isPowerOfTwo(n) || n < 2) {
    return;
  }

  int bits = 0;
  for (std::size_t tmp = n; tmp > 1; tmp >>= 1U) {
    ++bits;
  }

  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = reverseBits(i, bits);
    if (j > i) {
      std::swap(data[i], data[j]);
    }
  }

  constexpr float pi = 3.14159265358979323846f;
  for (std::size_t len = 2; len <= n; len <<= 1U) {
    const float ang = -2.0f * pi / static_cast<float>(len);
    const std::complex<float> wlen(std::cos(ang), std::sin(ang));

    for (std::size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      for (std::size_t j = 0; j < len / 2; ++j) {
        const std::complex<float> u = data[i + j];
        const std::complex<float> v = data[i + j + len / 2] * w;
        data[i + j] = u + v;
        data[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

std::vector<std::complex<float>> Fft::forwardReal(const std::vector<float>& input)
{
  std::vector<std::complex<float>> data;
  data.reserve(input.size());
  for (float v : input) {
    data.emplace_back(v, 0.0f);
  }
  forward(data);
  return data;
}

} // namespace dsp
