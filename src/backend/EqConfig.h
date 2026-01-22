#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

enum class EqBandType {
  Peaking = 0,
  LowShelf = 1,
  HighShelf = 2,
  LowPass = 3,
  HighPass = 4,
  Notch = 5,
  BandPass = 6,
};

struct EqBand final {
  bool enabled = true;
  EqBandType type = EqBandType::Peaking;
  double freqHz = 1000.0;
  double q = 1.0;
  double gainDb = 0.0;
};

struct EqPreset final {
  bool enabled = false;
  double preampDb = 0.0;
  QVector<EqBand> bands;
};

inline EqPreset defaultEqPreset(int bands = 6)
{
  EqPreset p;
  p.enabled = false;
  p.preampDb = 0.0;
  p.bands.reserve(std::max(0, bands));

  for (int i = 0; i < bands; ++i) {
    // A sensible default spread across the spectrum (log-spaced).
    constexpr double fMin = 60.0;
    constexpr double fMax = 12000.0;
    const double t = (bands <= 1) ? 0.5 : (static_cast<double>(i) / static_cast<double>(bands - 1));
    const double freq = fMin * std::pow(fMax / fMin, t);

    EqBand b;
    b.enabled = true;
    b.type = EqBandType::Peaking;
    b.freqHz = freq;
    b.q = 1.0;
    b.gainDb = 0.0;
    p.bands.push_back(b);
  }
  return p;
}

inline QString eqBandTypeToString(EqBandType t)
{
  switch (t) {
    case EqBandType::Peaking:
      return QStringLiteral("peaking");
    case EqBandType::LowShelf:
      return QStringLiteral("lowshelf");
    case EqBandType::HighShelf:
      return QStringLiteral("highshelf");
    case EqBandType::LowPass:
      return QStringLiteral("lowpass");
    case EqBandType::HighPass:
      return QStringLiteral("highpass");
    case EqBandType::Notch:
      return QStringLiteral("notch");
    case EqBandType::BandPass:
      return QStringLiteral("bandpass");
  }
  return QStringLiteral("peaking");
}

inline EqBandType eqBandTypeFromString(const QString& s)
{
  const QString k = s.trimmed().toLower();
  if (k == QStringLiteral("lowshelf")) {
    return EqBandType::LowShelf;
  }
  if (k == QStringLiteral("highshelf")) {
    return EqBandType::HighShelf;
  }
  if (k == QStringLiteral("lowpass")) {
    return EqBandType::LowPass;
  }
  if (k == QStringLiteral("highpass")) {
    return EqBandType::HighPass;
  }
  if (k == QStringLiteral("notch")) {
    return EqBandType::Notch;
  }
  if (k == QStringLiteral("bandpass")) {
    return EqBandType::BandPass;
  }
  return EqBandType::Peaking;
}

inline QJsonObject eqBandToJson(const EqBand& b)
{
  QJsonObject o;
  o.insert(QStringLiteral("enabled"), b.enabled);
  o.insert(QStringLiteral("type"), eqBandTypeToString(b.type));
  o.insert(QStringLiteral("freqHz"), b.freqHz);
  o.insert(QStringLiteral("q"), b.q);
  o.insert(QStringLiteral("gainDb"), b.gainDb);
  return o;
}

inline EqBand eqBandFromJson(const QJsonObject& o)
{
  EqBand b;
  b.enabled = o.value(QStringLiteral("enabled")).toBool(true);
  b.type = eqBandTypeFromString(o.value(QStringLiteral("type")).toString());
  b.freqHz = o.value(QStringLiteral("freqHz")).toDouble(1000.0);
  b.q = o.value(QStringLiteral("q")).toDouble(1.0);
  b.gainDb = o.value(QStringLiteral("gainDb")).toDouble(0.0);
  return b;
}

inline QJsonObject eqPresetToJson(const EqPreset& p)
{
  QJsonObject o;
  o.insert(QStringLiteral("enabled"), p.enabled);
  o.insert(QStringLiteral("preampDb"), p.preampDb);
  QJsonArray bands;
  for (const auto& b : p.bands) {
    bands.append(eqBandToJson(b));
  }
  o.insert(QStringLiteral("bands"), bands);
  return o;
}

inline EqPreset eqPresetFromJson(const QJsonObject& o)
{
  EqPreset p;
  p.enabled = o.value(QStringLiteral("enabled")).toBool(false);
  p.preampDb = o.value(QStringLiteral("preampDb")).toDouble(0.0);
  const QJsonArray bands = o.value(QStringLiteral("bands")).toArray();
  p.bands.reserve(bands.size());
  for (const auto& v : bands) {
    if (!v.isObject()) {
      continue;
    }
    p.bands.push_back(eqBandFromJson(v.toObject()));
  }
  if (p.bands.isEmpty()) {
    p = defaultEqPreset();
  }
  return p;
}
