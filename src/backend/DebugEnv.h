#pragma once

#include <QString>

#include <cstdint>
#include <cstdlib>

namespace headroom::debug {
inline bool envFlag(const char* key)
{
  return std::getenv(key) != nullptr;
}

inline QString envString(const char* key)
{
  const char* v = std::getenv(key);
  if (!v) {
    return {};
  }
  return QString::fromUtf8(v).trimmed();
}

inline bool pwOpsEnabled()
{
  static const bool enabled = envFlag("HEADROOM_DEBUG_PW_OPS");
  return enabled;
}

inline bool pwControlsEnabled()
{
  static const bool enabled = envFlag("HEADROOM_DEBUG_PW_CONTROLS");
  return enabled;
}

inline bool pwNodeFilterMatches(uint32_t nodeId, const QString& nodeName, const QString& nodeDescription)
{
  static const QString raw = envString("HEADROOM_DEBUG_PW_NODE");
  if (raw.isEmpty() || raw == QStringLiteral("*") || raw.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
    return true;
  }

  QString s = raw;
  if (s.startsWith(QStringLiteral("id:"), Qt::CaseInsensitive)) {
    s = s.mid(3).trimmed();
  }

  bool ok = false;
  const uint32_t wantedId = s.toUInt(&ok);
  if (ok) {
    return nodeId == wantedId;
  }

  if (!nodeName.isEmpty() && nodeName.contains(s, Qt::CaseInsensitive)) {
    return true;
  }
  if (!nodeDescription.isEmpty() && nodeDescription.contains(s, Qt::CaseInsensitive)) {
    return true;
  }
  return false;
}
} // namespace headroom::debug

