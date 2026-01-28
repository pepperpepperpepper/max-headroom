#include "tui/TuiInternal.h"

#include "settings/SettingsKeys.h"

#include <QDateTime>
#include <QMultiHash>
#include <QSet>

#include <algorithm>

namespace headroomtui {

QStringList defaultSinksOrder(const QList<PwNodeInfo>& sinks)
{
  QList<PwNodeInfo> sorted = sinks;
  std::sort(sorted.begin(), sorted.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
  });

  QStringList order;
  order.reserve(sorted.size());
  for (const auto& n : sorted) {
    if (!n.name.isEmpty()) {
      order.push_back(n.name);
    }
  }
  return order;
}

QList<PwNodeInfo> applySinksOrder(const QList<PwNodeInfo>& sinks, QSettings& s)
{
  const QStringList saved = s.value(SettingsKeys::sinksOrder()).toStringList();
  if (saved.isEmpty()) {
    QList<PwNodeInfo> sorted = sinks;
    std::sort(sorted.begin(), sorted.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
    });
    return sorted;
  }

  QHash<QString, PwNodeInfo> byName;
  byName.reserve(sinks.size());
  for (const auto& node : sinks) {
    byName.insert(node.name, node);
  }

  QStringList used;
  QList<PwNodeInfo> ordered;
  used.reserve(saved.size());
  ordered.reserve(sinks.size());

  for (const auto& name : saved) {
    if (!byName.contains(name)) {
      continue;
    }
    ordered.push_back(byName.value(name));
    used.push_back(name);
  }

  QList<PwNodeInfo> remaining;
  remaining.reserve(sinks.size());
  for (const auto& node : sinks) {
    if (!used.contains(node.name)) {
      remaining.push_back(node);
    }
  }
  std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
    return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
  });
  ordered.append(remaining);
  return ordered;
}

QList<PwNodeInfo> eqTargetsForGraph(PipeWireGraph* graph)
{
  QList<PwNodeInfo> out;
  if (!graph) {
    return out;
  }

  QSettings s;
  QList<PwNodeInfo> sinks = applySinksOrder(graph->audioSinks(), s);
  QList<PwNodeInfo> sources = graph->audioSources();
  QList<PwNodeInfo> playback = graph->audioPlaybackStreams();
  QList<PwNodeInfo> capture = graph->audioCaptureStreams();

  auto sortByLabel = [](const PwNodeInfo& a, const PwNodeInfo& b) { return displayNameForNode(a).toLower() < displayNameForNode(b).toLower(); };
  std::sort(sources.begin(), sources.end(), sortByLabel);
  std::sort(playback.begin(), playback.end(), sortByLabel);
  std::sort(capture.begin(), capture.end(), sortByLabel);

  out.reserve(sinks.size() + sources.size() + playback.size() + capture.size());
  out.append(sinks);
  out.append(sources);
  out.append(playback);
  out.append(capture);
  return out;
}

bool moveSinkInOrder(const QList<PwNodeInfo>& sinks, const QString& sinkName, int delta, QString* statusOut)
{
  if (sinkName.trimmed().isEmpty()) {
    if (statusOut) {
      *statusOut = QStringLiteral("No sink selected.");
    }
    return false;
  }

  QSettings s;
  QStringList order = s.value(SettingsKeys::sinksOrder()).toStringList();

  if (order.isEmpty()) {
    order = defaultSinksOrder(sinks);
  } else {
    const QSet<QString> known = QSet<QString>(order.begin(), order.end());
    QList<PwNodeInfo> remaining;
    remaining.reserve(sinks.size());
    for (const auto& n : sinks) {
      if (!known.contains(n.name)) {
        remaining.push_back(n);
      }
    }
    std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      return displayNameForNode(a).toLower() < displayNameForNode(b).toLower();
    });
    for (const auto& n : remaining) {
      order.push_back(n.name);
    }
  }

  int idx = order.indexOf(sinkName);
  if (idx < 0) {
    order.push_back(sinkName);
    idx = order.size() - 1;
  }

  const int next = idx + delta;
  if (next < 0 || next >= order.size()) {
    if (statusOut) {
      *statusOut = QStringLiteral("Already at %1.").arg(delta < 0 ? QStringLiteral("top") : QStringLiteral("bottom"));
    }
    return false;
  }

  order.swapItemsAt(idx, next);

  const QStringList def = defaultSinksOrder(sinks);
  const bool storeCustom = !def.isEmpty() && order != def;
  if (storeCustom) {
    s.setValue(SettingsKeys::sinksOrder(), order);
  } else {
    s.remove(SettingsKeys::sinksOrder());
  }

  if (statusOut) {
    *statusOut = QStringLiteral("Output order updated.");
  }
  return true;
}

PortKind portKindFor(const PwPortInfo& p, const QHash<uint32_t, PwNodeInfo>& nodesById)
{
  const QString mt = p.mediaType.trimmed().toLower();
  if (mt == QStringLiteral("midi")) {
    return PortKind::Midi;
  }
  if (mt == QStringLiteral("audio")) {
    return PortKind::Audio;
  }
  if (!p.audioChannel.isEmpty()) {
    return PortKind::Audio;
  }
  if (p.formatDsp.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (p.formatDsp.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  const auto it = nodesById.constFind(p.nodeId);
  if (it != nodesById.constEnd()) {
    if (it->mediaClass.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
      return PortKind::Midi;
    }
    if (it->mediaClass.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
      return PortKind::Audio;
    }
  }
  if (p.name.contains(QStringLiteral("midi"), Qt::CaseInsensitive) || p.alias.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  return PortKind::Other;
}

RecordingGraphSnapshot captureRecordingSnapshot(PipeWireGraph* graph)
{
  RecordingGraphSnapshot snap;
  snap.capturedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  if (!graph) {
    return snap;
  }

  snap.sinks = graph->audioSinks();
  snap.sources = graph->audioSources();
  snap.playbackStreams = graph->audioPlaybackStreams();
  snap.captureStreams = graph->audioCaptureStreams();
  snap.defaultSinkId = graph->defaultAudioSinkId();
  snap.defaultSourceId = graph->defaultAudioSourceId();
  return snap;
}

QVector<RecordingTarget> buildRecordingTargets(PipeWireGraph* graph)
{
  QVector<RecordingTarget> targets;
  targets.push_back({QStringLiteral("System mix (default output monitor)"), QString{}, true});

  if (graph) {
    const QList<PwNodeInfo> sinks = graph->audioSinks();
    for (const auto& s : sinks) {
      targets.push_back({QStringLiteral("Output: %1").arg(displayNameForNode(s)), s.name, true});
    }

    const QList<PwNodeInfo> playback = graph->audioPlaybackStreams();
    for (const auto& n : playback) {
      const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
      const QString name = displayNameForNode(n);
      const QString label = (!app.isEmpty() && app != name) ? QStringLiteral("%1 — %2").arg(app, name) : name;
      targets.push_back({QStringLiteral("App playback: %1").arg(label), n.name, false});
    }

    targets.push_back({QStringLiteral("Default input (mic)"), QString{}, false});

    const QList<PwNodeInfo> sources = graph->audioSources();
    for (const auto& s : sources) {
      targets.push_back({QStringLiteral("Input: %1").arg(displayNameForNode(s)), s.name, false});
    }

    const QList<PwNodeInfo> capture = graph->audioCaptureStreams();
    for (const auto& n : capture) {
      const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
      const QString name = displayNameForNode(n);
      const QString label = (!app.isEmpty() && app != name) ? QStringLiteral("%1 — %2").arg(app, name) : name;
      targets.push_back({QStringLiteral("App recording: %1").arg(label), n.name, true});
    }
  }

  return targets;
}

StreamRoute routeForStream(PipeWireGraph* graph, const PwNodeInfo& stream)
{
  if (!graph) {
    return {};
  }

  const bool isPlayback = stream.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"));

  const QList<PwNodeInfo> nodes = graph->nodes();
  const QList<PwLinkInfo> links = graph->links();

  QHash<uint32_t, PwNodeInfo> nodesById;
  nodesById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodesById.insert(n.id, n);
  }

  QMultiHash<uint32_t, uint32_t> forward;
  QMultiHash<uint32_t, uint32_t> backward;
  forward.reserve(links.size());
  backward.reserve(links.size());
  for (const auto& l : links) {
    if (l.outputNodeId != 0 && l.inputNodeId != 0) {
      forward.insert(l.outputNodeId, l.inputNodeId);
      backward.insert(l.inputNodeId, l.outputNodeId);
    }
  }

  const QString wanted = isPlayback ? QStringLiteral("Audio/Sink") : QStringLiteral("Audio/Source");

  struct QueueItem {
    uint32_t nodeId = 0;
    int depth = 0;
  };

  constexpr int kMaxDepth = 6;

  QList<QueueItem> queue;
  queue.reserve(64);
  QSet<uint32_t> visited;
  visited.reserve(64);

  visited.insert(stream.id);
  queue.push_back({stream.id, 0});

  while (!queue.isEmpty()) {
    const QueueItem item = queue.takeFirst();
    if (item.depth > kMaxDepth) {
      continue;
    }

    if (item.nodeId != stream.id) {
      const auto it = nodesById.constFind(item.nodeId);
      if (it != nodesById.constEnd() && it->mediaClass == wanted) {
        return StreamRoute{it->id, displayNameForNode(*it), isPlayback};
      }
    }

    if (item.depth == kMaxDepth) {
      continue;
    }

    const auto nexts = isPlayback ? forward.values(item.nodeId) : backward.values(item.nodeId);
    for (uint32_t next : nexts) {
      if (visited.contains(next)) {
        continue;
      }
      visited.insert(next);
      queue.push_back({next, item.depth + 1});
    }
  }

  return StreamRoute{0, {}, isPlayback};
}

} // namespace headroomtui
