#pragma once

#include <QString>
#include <QStringList>

struct SystemdUnitStatus final {
  QString unit;
  QString loadState;
  QString activeState;
  QString subState;
  QString description;
  QString error;

  bool exists() const { return loadState != QStringLiteral("not-found") && !loadState.isEmpty(); }
  bool isActive() const { return activeState == QStringLiteral("active"); }
};

class EngineControl final
{
public:
  static bool isSystemctlAvailable();
  static bool canTalkToUserSystemd(QString* error = nullptr);

  static QStringList defaultUserUnits();
  static QString normalizeUnitAlias(const QString& unitOrAlias);

  static SystemdUnitStatus queryUserUnit(const QString& unitOrAlias, QString* error = nullptr);

  static bool startUserUnit(const QString& unitOrAlias, QString* error = nullptr);
  static bool stopUserUnit(const QString& unitOrAlias, QString* error = nullptr);
  static bool restartUserUnit(const QString& unitOrAlias, QString* error = nullptr);

private:
  static bool runSystemctl(const QStringList& args, QString* stdoutText, QString* stderrText, int* exitCode);
};

