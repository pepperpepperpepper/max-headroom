#include "AppTheme.h"

#include "settings/SettingsKeys.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace AppTheme {
Mode parseMode(const QString& raw)
{
  const QString s = raw.trimmed().toLower();
  if (s == QStringLiteral("dark")) {
    return Mode::Dark;
  }
  if (s == QStringLiteral("light")) {
    return Mode::Light;
  }
  return Mode::System;
}

QString modeToString(Mode mode)
{
  switch (mode) {
    case Mode::Dark:
      return QStringLiteral("dark");
    case Mode::Light:
      return QStringLiteral("light");
    case Mode::System:
    default:
      return QStringLiteral("system");
  }
}

Mode load(QSettings& settings)
{
  return parseMode(settings.value(SettingsKeys::uiTheme()).toString());
}

void save(QSettings& settings, Mode mode)
{
  if (mode == Mode::System) {
    settings.remove(SettingsKeys::uiTheme());
    return;
  }
  settings.setValue(SettingsKeys::uiTheme(), modeToString(mode));
}

void apply(Mode mode)
{
  static bool captured = false;
  static QPalette basePalette;
  static QString baseStyle;

  if (!captured) {
    captured = true;
    basePalette = QApplication::palette();
    baseStyle = QApplication::style() ? QApplication::style()->objectName() : QString{};
  }

  if (mode == Mode::System) {
    if (!baseStyle.isEmpty()) {
      QApplication::setStyle(baseStyle);
    }
    QApplication::setPalette(basePalette);
    return;
  }

  QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  if (mode == Mode::Light) {
    if (QApplication::style()) {
      QApplication::setPalette(QApplication::style()->standardPalette());
    }
    return;
  }

  QPalette dark;
  dark.setColor(QPalette::Window, QColor(17, 24, 39));
  dark.setColor(QPalette::WindowText, QColor(226, 232, 240));
  dark.setColor(QPalette::Base, QColor(15, 23, 42));
  dark.setColor(QPalette::AlternateBase, QColor(30, 41, 59));
  dark.setColor(QPalette::ToolTipBase, QColor(226, 232, 240));
  dark.setColor(QPalette::ToolTipText, QColor(15, 23, 42));
  dark.setColor(QPalette::Text, QColor(226, 232, 240));
  dark.setColor(QPalette::Button, QColor(30, 41, 59));
  dark.setColor(QPalette::ButtonText, QColor(226, 232, 240));
  dark.setColor(QPalette::BrightText, QColor(239, 68, 68));
  dark.setColor(QPalette::Link, QColor(125, 211, 252));
  dark.setColor(QPalette::Highlight, QColor(59, 130, 246));
  dark.setColor(QPalette::HighlightedText, QColor(15, 23, 42));

  QApplication::setPalette(dark);
}

void applyFromSettings()
{
  QSettings s;
  apply(load(s));
}
} // namespace AppTheme

