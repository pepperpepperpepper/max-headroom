#include "PatchbayPortConfig.h"

#include "settings/SettingsKeys.h"

#include <QSettings>

namespace {
bool endpointValid(const QString& nodeName, const QString& portName)
{
  return !nodeName.trimmed().isEmpty() && !portName.trimmed().isEmpty();
}
} // namespace

PatchbayPortConfig PatchbayPortConfigStore::load(QSettings& s, const QString& nodeName, const QString& portName)
{
  PatchbayPortConfig cfg;
  cfg.customAlias = customAlias(s, nodeName, portName);
  cfg.locked = isLocked(s, nodeName, portName);
  return cfg;
}

std::optional<QString> PatchbayPortConfigStore::customAlias(QSettings& s, const QString& nodeName, const QString& portName)
{
  if (!endpointValid(nodeName, portName)) {
    return std::nullopt;
  }
  const QString v = s.value(SettingsKeys::patchbayPortAliasKeyForNodePort(nodeName, portName)).toString().trimmed();
  if (v.isEmpty()) {
    return std::nullopt;
  }
  return v;
}

void PatchbayPortConfigStore::setCustomAlias(QSettings& s, const QString& nodeName, const QString& portName, const QString& alias)
{
  if (!endpointValid(nodeName, portName)) {
    return;
  }
  const QString v = alias.trimmed();
  if (v.isEmpty()) {
    clearCustomAlias(s, nodeName, portName);
    return;
  }
  s.setValue(SettingsKeys::patchbayPortAliasKeyForNodePort(nodeName, portName), v);
}

void PatchbayPortConfigStore::clearCustomAlias(QSettings& s, const QString& nodeName, const QString& portName)
{
  if (!endpointValid(nodeName, portName)) {
    return;
  }
  s.remove(SettingsKeys::patchbayPortAliasKeyForNodePort(nodeName, portName));
}

bool PatchbayPortConfigStore::isLocked(QSettings& s, const QString& nodeName, const QString& portName)
{
  if (!endpointValid(nodeName, portName)) {
    return false;
  }
  return s.value(SettingsKeys::patchbayPortLockKeyForNodePort(nodeName, portName)).toBool();
}

void PatchbayPortConfigStore::setLocked(QSettings& s, const QString& nodeName, const QString& portName, bool locked)
{
  if (!endpointValid(nodeName, portName)) {
    return;
  }
  s.setValue(SettingsKeys::patchbayPortLockKeyForNodePort(nodeName, portName), locked);
}

void PatchbayPortConfigStore::clearAll(QSettings& s)
{
  s.remove(SettingsKeys::patchbayPortAliasesGroup());
  s.remove(SettingsKeys::patchbayPortLocksGroup());
}

