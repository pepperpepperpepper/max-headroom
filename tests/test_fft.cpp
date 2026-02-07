#include <QtTest/QtTest>

#include "dsp/Fft.h"

#include <cmath>

class TestFft final : public QObject
{
  Q_OBJECT

private slots:
  void isPowerOfTwo();
  void hannWindow_basic();
  void forwardReal_impulse();
  void forward_nonPowerOfTwo_noop();
};

void TestFft::isPowerOfTwo()
{
  QVERIFY(!dsp::Fft::isPowerOfTwo(0));
  QVERIFY(dsp::Fft::isPowerOfTwo(1));
  QVERIFY(dsp::Fft::isPowerOfTwo(2));
  QVERIFY(!dsp::Fft::isPowerOfTwo(3));
  QVERIFY(dsp::Fft::isPowerOfTwo(1024));
}

void TestFft::hannWindow_basic()
{
  const std::vector<float> w0 = dsp::Fft::hannWindow(0);
  QCOMPARE(w0.size(), std::size_t{0});

  const std::vector<float> w8 = dsp::Fft::hannWindow(8);
  QCOMPARE(w8.size(), std::size_t{8});

  QVERIFY(std::abs(w8.front()) < 1e-6f);
  QVERIFY(std::abs(w8.back()) < 1e-6f);
  QVERIFY(std::abs(w8[1] - w8[6]) < 1e-6f);
  QVERIFY(std::abs(w8[2] - w8[5]) < 1e-6f);
}

void TestFft::forwardReal_impulse()
{
  std::vector<float> input(8, 0.0f);
  input[0] = 1.0f;

  const std::vector<std::complex<float>> spectrum = dsp::Fft::forwardReal(input);
  QCOMPARE(spectrum.size(), std::size_t{8});

  for (const auto& v : spectrum) {
    QVERIFY(std::abs(v.real() - 1.0f) < 1e-5f);
    QVERIFY(std::abs(v.imag()) < 1e-5f);
  }
}

void TestFft::forward_nonPowerOfTwo_noop()
{
  std::vector<std::complex<float>> data = {
      { 1.0f, 0.0f },
      { 2.0f, 0.0f },
      { 3.0f, 0.0f },
      { 4.0f, 0.0f },
      { 5.0f, 0.0f },
      { 6.0f, 0.0f },
  };
  const auto original = data;

  dsp::Fft::forward(data);
  QCOMPARE(data.size(), original.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    QCOMPARE(data[i].real(), original[i].real());
    QCOMPARE(data[i].imag(), original[i].imag());
  }
}

QTEST_APPLESS_MAIN(TestFft)

#include "test_fft.moc"
