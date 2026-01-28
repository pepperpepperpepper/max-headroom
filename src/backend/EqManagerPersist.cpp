#include "EqManager.h"

#include <QJsonDocument>
#include <QSettings>

QString EqManager::presetKey(const QString& nodeName) const
{
  return QStringLiteral("eq/%1/presetJson").arg(nodeName);
}

EqPreset EqManager::loadPreset(const QString& nodeName) const
{
  QSettings s;
  const QString json = s.value(presetKey(nodeName)).toString();
  if (json.trimmed().isEmpty()) {
    return defaultEqPreset(6);
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return defaultEqPreset(6);
  }

  return eqPresetFromJson(doc.object());
}

void EqManager::savePreset(const QString& nodeName, const EqPreset& preset)
{
  QSettings s;
  const QJsonDocument doc(eqPresetToJson(preset));
  s.setValue(presetKey(nodeName), QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

