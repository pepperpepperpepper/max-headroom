#pragma once

#include <QString>

#include <optional>

class QSettings;

struct PatchbayPortConfig final {
  std::optional<QString> customAlias;
  bool locked = false;
};

class PatchbayPortConfigStore final
{
public:
  static PatchbayPortConfig load(QSettings& s, const QString& nodeName, const QString& portName);

  static std::optional<QString> customAlias(QSettings& s, const QString& nodeName, const QString& portName);
  static void setCustomAlias(QSettings& s, const QString& nodeName, const QString& portName, const QString& alias);
  static void clearCustomAlias(QSettings& s, const QString& nodeName, const QString& portName);

  static bool isLocked(QSettings& s, const QString& nodeName, const QString& portName);
  static void setLocked(QSettings& s, const QString& nodeName, const QString& portName, bool locked);

  static void clearAll(QSettings& s);
};

