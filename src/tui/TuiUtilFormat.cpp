#include "tui/TuiInternal.h"

#include <cmath>

namespace headroomtui {

QString displayNameForNode(const PwNodeInfo& n)
{
  if (!n.description.isEmpty()) {
    return n.description;
  }
  if (!n.name.isEmpty()) {
    return n.name;
  }
  return QStringLiteral("(unnamed)");
}

const char* portKindLabelShort(PortKind k)
{
  switch (k) {
  case PortKind::Audio:
    return "AUD";
  case PortKind::Midi:
    return "MIDI";
  case PortKind::Other:
    return "OTH";
  }
  return "OTH";
}

static QString formatSnapshotNodeLine(const PwNodeInfo& n)
{
  const QString base = displayNameForNode(n);
  const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
  if (!app.isEmpty() && app != base) {
    return QStringLiteral("%1  %2 — %3  (%4)").arg(n.id, 4).arg(app, base, n.mediaClass);
  }
  return QStringLiteral("%1  %2  (%3)").arg(n.id, 4).arg(base, n.mediaClass);
}

QStringList recordingSnapshotLines(const RecordingGraphSnapshot& snap)
{
  QStringList lines;
  lines.push_back(QStringLiteral("Captured at (UTC): %1").arg(snap.capturedAtUtc));
  lines.push_back(QStringLiteral("Default sink id: %1").arg(snap.defaultSinkId ? QString::number(*snap.defaultSinkId) : QStringLiteral("(unknown)")));
  lines.push_back(
      QStringLiteral("Default source id: %1").arg(snap.defaultSourceId ? QString::number(*snap.defaultSourceId) : QStringLiteral("(unknown)")));
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Output devices (%1):").arg(snap.sinks.size()));
  for (const auto& n : snap.sinks) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Input devices (%1):").arg(snap.sources.size()));
  for (const auto& n : snap.sources) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Playback streams (%1):").arg(snap.playbackStreams.size()));
  for (const auto& n : snap.playbackStreams) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }
  lines.push_back(QString());

  lines.push_back(QStringLiteral("Capture streams (%1):").arg(snap.captureStreams.size()));
  for (const auto& n : snap.captureStreams) {
    lines.push_back(QStringLiteral("  %1").arg(formatSnapshotNodeLine(n)));
  }

  return lines;
}

QString nodeSummary(const QString& prefix, const PwNodeInfo& n, const PwNodeControls& c, bool showName)
{
  if (n.id == 0u) {
    return QStringLiteral("%1: (none)").arg(prefix);
  }

  const int pct = static_cast<int>(std::round(c.volume * 100.0f));
  const QString vol = QStringLiteral("%1%").arg(pct);
  const QString mute = c.mute ? QStringLiteral("muted") : QStringLiteral("unmuted");
  const QString def = showName ? QStringLiteral(" (default)") : QString();

  return QStringLiteral("%1 %2  %3  %4  %5%6").arg(prefix).arg(n.id).arg(displayNameForNode(n)).arg(vol).arg(mute).arg(def);
}

} // namespace headroomtui
