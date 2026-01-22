#pragma once

#include <QString>

namespace SettingsKeys {
inline QString sinksOrder()
{
  return QStringLiteral("patchbay/sinksOrder");
}

inline QString visualizerRefreshIntervalMs()
{
  return QStringLiteral("visualizer/refreshIntervalMs");
}

inline QString visualizerFftSize()
{
  return QStringLiteral("visualizer/fftSize");
}

inline QString visualizerSpectrumSmoothing()
{
  return QStringLiteral("visualizer/spectrumSmoothing");
}

inline QString visualizerWaveformHistorySeconds()
{
  return QStringLiteral("visualizer/waveformHistorySeconds");
}

inline QString visualizerSpectrogramHistorySeconds()
{
  return QStringLiteral("visualizer/spectrogramHistorySeconds");
}

inline QString patchbayLayoutEditMode()
{
  return QStringLiteral("patchbay/layoutEditMode");
}

inline QString patchbayLayoutPositionsGroup()
{
  return QStringLiteral("patchbay/layout/positions");
}

inline QString patchbaySelectedProfileName()
{
  return QStringLiteral("patchbay/selectedProfileName");
}

inline QString patchbayActiveProfileName()
{
  return QStringLiteral("patchbay/activeProfileName");
}

inline QString patchbayProfilesGroup()
{
  return QStringLiteral("patchbay/profiles");
}

inline QString patchbayProfileHooksGroup()
{
  return QStringLiteral("patchbay/profileHooks");
}

inline QString patchbayAutoConnectEnabled()
{
  return QStringLiteral("patchbay/autoConnectEnabled");
}

inline QString patchbayAutoConnectWhitelist()
{
  return QStringLiteral("patchbay/autoConnectWhitelist");
}

inline QString patchbayAutoConnectBlacklist()
{
  return QStringLiteral("patchbay/autoConnectBlacklist");
}

inline QString patchbayAutoConnectRulesGroup()
{
  return QStringLiteral("patchbay/autoConnect/rules");
}

inline QString patchbayPortAliasesGroup()
{
  return QStringLiteral("patchbay/portAliases");
}

inline QString patchbayPortLocksGroup()
{
  return QStringLiteral("patchbay/portLocks");
}

inline QString sessionsSnapshotsGroup()
{
  return QStringLiteral("sessions/snapshots");
}

inline QString patchbayLayoutPositionKeyForNodeName(const QString& nodeName)
{
  const QByteArray enc = nodeName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QStringLiteral("%1/%2").arg(patchbayLayoutPositionsGroup(), QString::fromUtf8(enc));
}

inline QString patchbayEndpointKeyForNodePort(const QString& nodeName, const QString& portName)
{
  const QString raw = QStringLiteral("%1\n%2").arg(nodeName, portName);
  const QByteArray enc = raw.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

inline QString patchbayPortAliasKeyForNodePort(const QString& nodeName, const QString& portName)
{
  return QStringLiteral("%1/%2").arg(patchbayPortAliasesGroup(), patchbayEndpointKeyForNodePort(nodeName, portName));
}

inline QString patchbayPortLockKeyForNodePort(const QString& nodeName, const QString& portName)
{
  return QStringLiteral("%1/%2").arg(patchbayPortLocksGroup(), patchbayEndpointKeyForNodePort(nodeName, portName));
}
} // namespace SettingsKeys
