#pragma once

#include <QString>

class QSettings;

namespace AppTheme {
enum class Mode {
  System = 0,
  Light,
  Dark,
};

Mode parseMode(const QString& raw);
QString modeToString(Mode mode);

Mode load(QSettings& settings);
void save(QSettings& settings, Mode mode);

void apply(Mode mode);
void applyFromSettings();
} // namespace AppTheme

