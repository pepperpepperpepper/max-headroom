#include <QtTest/QtTest>

#include "backend/VolumeScale.h"

#include <cmath>

class TestVolumeScale final : public QObject
{
  Q_OBJECT

private slots:
  void uiPercentToLinear_basic();
  void uiPercentToLinear_above100();
  void linearToUiPercent_basic();
  void roundTrip_basic();
};

void TestVolumeScale::uiPercentToLinear_basic()
{
  QCOMPARE(headroom::volume::uiPercentToLinear(-1), 0.0f);
  QCOMPARE(headroom::volume::uiPercentToLinear(0), 0.0f);

  QVERIFY(std::abs(headroom::volume::uiPercentToLinear(50) - 0.125f) < 1e-6f);
  QVERIFY(std::abs(headroom::volume::uiPercentToLinear(100) - 1.0f) < 1e-6f);
}

void TestVolumeScale::uiPercentToLinear_above100()
{
  const float vol120 = headroom::volume::uiPercentToLinear(120);
  QVERIFY(vol120 > 1.0f);
  QVERIFY(std::abs(vol120 - (1.2f * 1.2f * 1.2f)) < 1e-6f);

  const float vol150 = headroom::volume::uiPercentToLinear(150);
  QVERIFY(std::abs(vol150 - 3.375f) < 1e-6f);

  QCOMPARE(headroom::volume::uiPercentToLinear(153), headroom::volume::kUiMaxLinear);
  QCOMPARE(headroom::volume::uiPercentToLinear(200), headroom::volume::kUiMaxLinear);
}

void TestVolumeScale::linearToUiPercent_basic()
{
  QCOMPARE(headroom::volume::linearToUiPercent(-1.0f), 0);
  QCOMPARE(headroom::volume::linearToUiPercent(0.0f), 0);

  QCOMPARE(headroom::volume::linearToUiPercent(0.125f), 50);
  QCOMPARE(headroom::volume::linearToUiPercent(1.0f), 100);
  QCOMPARE(headroom::volume::linearToUiPercent(3.375f), 150);
  QCOMPARE(headroom::volume::linearToUiPercent(headroom::volume::kUiMaxLinear), headroom::volume::kUiMaxPercent);
}

void TestVolumeScale::roundTrip_basic()
{
  for (int pct : {0, 1, 2, 50, 75, 99, 100, 101, 120, 150, 152, 153}) {
    const float lin = headroom::volume::uiPercentToLinear(pct);
    const int pct2 = headroom::volume::linearToUiPercent(lin);
    QVERIFY(pct2 >= 0);
    QVERIFY(pct2 <= headroom::volume::kUiMaxPercent);

    if (pct == 153) {
      QCOMPARE(pct2, 153);
    } else {
      QVERIFY(std::abs(pct2 - pct) <= 1);
    }
  }
}

QTEST_APPLESS_MAIN(TestVolumeScale)

#include "test_volume_scale.moc"

