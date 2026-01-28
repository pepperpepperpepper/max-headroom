#include "EqManager.h"

#include "backend/ParametricEqFilter.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"

#include <QRegularExpression>

#include <algorithm>

namespace {
bool isTargetClass(const QString& mediaClass)
{
  if (mediaClass == QStringLiteral("Audio/Sink") || mediaClass == QStringLiteral("Audio/Source")) {
    return true;
  }
  // Treat per-app streams as EQ targets too (per-stream EQ).
  if (mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
    return true;
  }
  if (mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
    return true;
  }
  return false;
}

bool isSinkLike(const QString& mediaClass)
{
  if (mediaClass == QStringLiteral("Audio/Sink")) {
    return true;
  }
  if (mediaClass == QStringLiteral("Audio/Source")) {
    return false;
  }
  // PipeWire streams:
  // - Stream/Output/Audio => source-like (outputs audio)
  // - Stream/Input/Audio  => sink-like (consumes audio)
  if (mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
    return true;
  }
  if (mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
    return false;
  }

	return false;
}

QString canonicalChannelLabelFromName(const QString& name)
{
  const QString n = name.trimmed().toUpper();
  const QStringList parts = n.split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")), Qt::SkipEmptyParts);
  for (const auto& part : parts) {
    if (part == QStringLiteral("FL") || part == QStringLiteral("FR") || part == QStringLiteral("FC") || part == QStringLiteral("LFE")
        || part == QStringLiteral("RL") || part == QStringLiteral("RR") || part == QStringLiteral("SL") || part == QStringLiteral("SR")
        || part == QStringLiteral("MONO")) {
      return part;
    }
  }
  return {};
}

QString portKeyForNode(const PwPortInfo& p)
{
  if (!p.audioChannel.isEmpty()) {
    return p.audioChannel;
  }
  const QString canonical = canonicalChannelLabelFromName(p.name);
  if (!canonical.isEmpty()) {
    return canonical;
  }
  return p.name;
}

QString stripDirectionalPrefix(const QString& name)
{
  if (name.startsWith(QStringLiteral("in_"))) {
    return name.mid(3);
  }
  if (name.startsWith(QStringLiteral("out_"))) {
    return name.mid(4);
  }
  return name;
}

QVector<QString> parseAudioPosition(const QString& audioPosition)
{
  QVector<QString> out;
  const QString n = audioPosition.trimmed().toUpper();
  const QStringList parts = n.split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")), Qt::SkipEmptyParts);
  for (const auto& part : parts) {
    const QString canonical = canonicalChannelLabelFromName(part);
    if (!canonical.isEmpty()) {
      out.push_back(canonical);
    }
  }
  return out;
}

QVector<QString> normalizeAudioPosition(QVector<QString> position, uint32_t channels)
{
  if (channels == 0) {
    return position;
  }

  const QVector<QString> fallback = {QStringLiteral("FL"),  QStringLiteral("FR"),  QStringLiteral("FC"),  QStringLiteral("LFE"),
                                     QStringLiteral("RL"),  QStringLiteral("RR"),  QStringLiteral("SL"),  QStringLiteral("SR"),
                                     QStringLiteral("MONO")};

  const int target = static_cast<int>(channels);
  if (position.size() > target) {
    position.resize(target);
    return position;
  }

  for (const auto& c : fallback) {
    if (position.size() >= target) {
      break;
    }
    if (!position.contains(c)) {
      position.push_back(c);
    }
  }
  while (position.size() < target) {
    position.push_back(QStringLiteral("MONO"));
  }
  return position;
}

QHash<QString, PwPortInfo> portsByChannel(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction, bool stripInOutPrefix)
{
  QHash<QString, PwPortInfo> out;
  for (const auto& p : ports) {
    if (p.nodeId != nodeId) {
      continue;
    }
    if (p.direction != direction) {
      continue;
    }
    QString key;
    if (stripInOutPrefix && p.audioChannel.isEmpty()) {
      key = stripDirectionalPrefix(p.name);
      const QString canonical = canonicalChannelLabelFromName(key);
      if (!canonical.isEmpty()) {
        key = canonical;
      }
    } else {
      key = portKeyForNode(p);
    }
    out.insert(key, p);
  }
  return out;
}

QVector<ParametricEqFilter::PortSpec> targetPortSpecs(PipeWireGraph* graph, const PwNodeInfo& node)
{
  QVector<ParametricEqFilter::PortSpec> out;
  if (!graph) {
    return out;
  }

  const QList<PwPortInfo> ports = graph->ports();
  const QString dir = isSinkLike(node.mediaClass) ? QStringLiteral("in") : QStringLiteral("out");
  QVector<PwPortInfo> matching;
  matching.reserve(ports.size());

  bool anyChannelProps = false;
  for (const auto& p : ports) {
    if (p.nodeId != node.id || p.direction != dir) {
      continue;
    }
    matching.push_back(p);
    if (!p.audioChannel.isEmpty()) {
      anyChannelProps = true;
    }
  }
  if (matching.isEmpty()) {
    return out;
  }

  const QVector<QString> rawLayout = parseAudioPosition(node.audioPosition);

  uint32_t chCount = 0;
  bool chCountKnown = false;

  if (node.audioChannels > 0) {
    chCount = node.audioChannels;
    chCountKnown = true;
  }
  if (chCount == 0) {
    const auto controls = graph->nodeControls(node.id);
    if (controls && !controls->channelVolumes.isEmpty()) {
      chCount = static_cast<uint32_t>(controls->channelVolumes.size());
      chCountKnown = true;
    }
  }
  if (chCount == 0) {
    if (!rawLayout.isEmpty()) {
      chCount = static_cast<uint32_t>(rawLayout.size());
      chCountKnown = true;
    }
  }
  if (chCount == 0) {
    chCount = matching.size() == 1 ? 2u : static_cast<uint32_t>(matching.size());
  }

  QVector<QString> layout = normalizeAudioPosition(rawLayout, chCount);
  if (layout.isEmpty()) {
    layout = normalizeAudioPosition({}, chCount);
  }

  if (!anyChannelProps && matching.size() == 1 && chCount > 1 && chCountKnown) {
    ParametricEqFilter::PortSpec spec;
    spec.key = portKeyForNode(matching[0]);
    spec.channels = layout;
    out.push_back(spec);
    return out;
  }

  out.reserve(matching.size());
  for (const auto& p : matching) {
    ParametricEqFilter::PortSpec spec;
    spec.key = portKeyForNode(p);

    QString channel = p.audioChannel;
    if (channel.isEmpty()) {
      channel = canonicalChannelLabelFromName(p.name);
    }
    if (channel.isEmpty() && chCount == 1 && !layout.isEmpty()) {
      channel = layout[0];
    }
    if (channel.isEmpty()) {
      channel = spec.key;
    }

    spec.channels = {channel};
    out.push_back(spec);
  }
  return out;
}
} // namespace

void EqManager::onGraphChanged()
{
  scheduleReconcile();
}

void EqManager::scheduleReconcile()
{
  if (m_reconcileScheduled) {
    return;
  }
  m_reconcileScheduled = true;
  QMetaObject::invokeMethod(this, &EqManager::reconcileAll, Qt::QueuedConnection);
}

QString EqManager::nodeLabel(const PwNodeInfo& node)
{
  return node.description.isEmpty() ? node.name : node.description;
}

QString EqManager::linkKey(uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort)
{
  return QStringLiteral("%1:%2->%3:%4").arg(outNode).arg(outPort).arg(inNode).arg(inPort);
}

bool EqManager::linkExists(const QList<PwLinkInfo>& links, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort)
{
  for (const auto& l : links) {
    if (l.outputNodeId == outNode && l.outputPortId == outPort && l.inputNodeId == inNode && l.inputPortId == inPort) {
      return true;
    }
  }
  return false;
}

void EqManager::reconcileAll()
{
  m_reconcileScheduled = false;

  if (!m_graph || !m_pw) {
    return;
  }

  const QList<PwNodeInfo> nodes = m_graph->nodes();

  // Deactivate EQs whose target nodes disappeared.
  for (auto it = m_activeByNodeName.begin(); it != m_activeByNodeName.end();) {
    const QString name = it.key();
    const bool exists = std::any_of(nodes.begin(), nodes.end(), [&](const PwNodeInfo& n) { return n.name == name; });
    if (!exists) {
      deactivate(it.value());
      it = m_activeByNodeName.erase(it);
      continue;
    }
    ++it;
  }

  // Activate any nodes that have EQ enabled in settings.
  for (const auto& node : nodes) {
    if (!isTargetClass(node.mediaClass)) {
      continue;
    }
    const EqPreset preset = loadPreset(node.name);
    if (!preset.enabled) {
      continue;
    }
    if (m_activeByNodeName.contains(node.name)) {
      continue;
    }

    ActiveEq eq;
    eq.targetName = node.name;
    eq.targetMediaClass = node.mediaClass;
    eq.targetId = node.id;
    eq.preset = preset;

    const QVector<ParametricEqFilter::PortSpec> ports = targetPortSpecs(m_graph, node);
    if (ports.isEmpty()) {
      continue;
    }

    QString filterNodeName = QStringLiteral("headroom.eq.%1").arg(node.name);
    filterNodeName.replace('/', '_');
    filterNodeName.replace(' ', '_');
    QString filterDesc = tr("EQ — %1").arg(nodeLabel(node));

    eq.filter = new ParametricEqFilter(m_pw, filterNodeName, filterDesc, ports, this);
    eq.filterNodeId = eq.filter ? eq.filter->nodeId() : 0;
    if (eq.filter) {
      eq.filter->setPreset(preset);
      eq.filterNodeId = eq.filter->nodeId();
    }

    m_activeByNodeName.insert(node.name, eq);
  }

  // Reconcile wiring for all active EQs.
  for (auto it = m_activeByNodeName.begin(); it != m_activeByNodeName.end(); ++it) {
    // Refresh targetId and preset (in case user edited settings).
    const QString name = it.key();
    for (const auto& node : nodes) {
      if (node.name != name) {
        continue;
      }
      it.value().targetId = node.id;
      it.value().targetMediaClass = node.mediaClass;
      break;
    }
    it.value().preset = loadPreset(name);
    if (it.value().filter) {
      it.value().filter->setPreset(it.value().preset);
      it.value().filterNodeId = it.value().filter->nodeId();
    }

    if (!it.value().preset.enabled) {
      deactivate(it.value());
      ParametricEqFilter* f = it.value().filter;
      it.value().filter = nullptr;
      if (f) {
        delete f;
      }
      // remove disabled eq from map after loop
    } else {
      reconcileOne(it.value());
    }
  }

  // Remove disabled EQ entries.
  for (auto it = m_activeByNodeName.begin(); it != m_activeByNodeName.end();) {
    if (!it.value().preset.enabled) {
      it = m_activeByNodeName.erase(it);
    } else {
      ++it;
    }
  }
}

void EqManager::deactivate(ActiveEq& eq)
{
  if (!m_graph) {
    return;
  }

  const uint32_t filterNodeId = eq.filterNodeId;
  const auto links = m_graph->links();

  // Remove any links involving the EQ node.
  if (filterNodeId != 0) {
    for (const auto& l : links) {
      if (l.outputNodeId == filterNodeId || l.inputNodeId == filterNodeId) {
        m_graph->destroyLink(l.id);
      }
    }
  }

  // Restore saved links.
  for (const auto& l : eq.savedLinks) {
    m_graph->createLink(l.outputNodeId, l.outputPortId, l.inputNodeId, l.inputPortId);
  }

  eq.savedLinks.clear();
  eq.savedLinkKeys.clear();
}

void EqManager::reconcileOne(ActiveEq& eq)
{
  if (!m_graph || !eq.filter || eq.filterNodeId == 0 || eq.targetId == 0) {
    return;
  }

  const uint32_t targetId = eq.targetId;
  const uint32_t filterId = eq.filterNodeId;
  const auto ports = m_graph->ports();
  const auto links = m_graph->links();

  const bool isSink = isSinkLike(eq.targetMediaClass);

  const QString targetDir = isSink ? QStringLiteral("in") : QStringLiteral("out");
  const QHash<QString, PwPortInfo> targetPorts = portsByChannel(ports, targetId, targetDir, false);
  const QHash<QString, PwPortInfo> filterIn = portsByChannel(ports, filterId, QStringLiteral("in"), true);
  const QHash<QString, PwPortInfo> filterOut = portsByChannel(ports, filterId, QStringLiteral("out"), true);

  if (targetPorts.isEmpty() || filterIn.isEmpty() || filterOut.isEmpty()) {
    return;
  }

  if (isSink) {
    // Ensure EQ output -> sink.
    for (const auto& sinkPort : targetPorts) {
      const QString ch = portKeyForNode(sinkPort);
      const PwPortInfo eqOutPort = filterOut.value(ch, filterOut.begin().value());
      if (!linkExists(links, filterId, eqOutPort.id, targetId, sinkPort.id)) {
        m_graph->createLink(filterId, eqOutPort.id, targetId, sinkPort.id);
      }
    }

    // Reroute any non-EQ links into the sink to go through EQ.
    for (const auto& l : links) {
      if (l.inputNodeId != targetId) {
        continue;
      }
      if (l.outputNodeId == filterId) {
        continue;
      }

      // Find channel for the sink input port.
      QString ch;
      for (auto it = targetPorts.begin(); it != targetPorts.end(); ++it) {
        if (it.value().id == l.inputPortId) {
          ch = it.key();
          break;
        }
      }
      if (ch.isEmpty()) {
        continue;
      }

      const PwPortInfo eqInPort = filterIn.value(ch, filterIn.begin().value());

      // Save original link for restoration.
      const QString k = linkKey(l.outputNodeId, l.outputPortId, l.inputNodeId, l.inputPortId);
      if (!eq.savedLinkKeys.contains(k)) {
        eq.savedLinkKeys.insert(k);
        eq.savedLinks.push_back(SavedLink{l.outputNodeId, l.outputPortId, l.inputNodeId, l.inputPortId});
      }

      m_graph->destroyLink(l.id);
      m_graph->createLink(l.outputNodeId, l.outputPortId, filterId, eqInPort.id);
    }
  } else {
    // Ensure source -> EQ input.
    for (const auto& srcPort : targetPorts) {
      const QString ch = portKeyForNode(srcPort);
      const PwPortInfo eqInPort = filterIn.value(ch, filterIn.begin().value());
      if (!linkExists(links, targetId, srcPort.id, filterId, eqInPort.id)) {
        m_graph->createLink(targetId, srcPort.id, filterId, eqInPort.id);
      }
    }

    // Reroute any non-EQ links out of the source to come from EQ output.
    for (const auto& l : links) {
      if (l.outputNodeId != targetId) {
        continue;
      }
      if (l.inputNodeId == filterId) {
        continue;
      }

      // Find channel for the source output port.
      QString ch;
      for (auto it = targetPorts.begin(); it != targetPorts.end(); ++it) {
        if (it.value().id == l.outputPortId) {
          ch = it.key();
          break;
        }
      }
      if (ch.isEmpty()) {
        continue;
      }

      const PwPortInfo eqOutPort = filterOut.value(ch, filterOut.begin().value());

      const QString k = linkKey(l.outputNodeId, l.outputPortId, l.inputNodeId, l.inputPortId);
      if (!eq.savedLinkKeys.contains(k)) {
        eq.savedLinkKeys.insert(k);
        eq.savedLinks.push_back(SavedLink{l.outputNodeId, l.outputPortId, l.inputNodeId, l.inputPortId});
      }

      m_graph->destroyLink(l.id);
      m_graph->createLink(filterId, eqOutPort.id, l.inputNodeId, l.inputPortId);
    }
  }
}

