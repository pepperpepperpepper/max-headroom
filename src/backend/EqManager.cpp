#include "EqManager.h"

#include "backend/ParametricEqFilter.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"

#include <QJsonDocument>
#include <QSettings>

#include <algorithm>

namespace {
bool isTargetClass(const QString& mediaClass)
{
  return mediaClass == QStringLiteral("Audio/Sink") || mediaClass == QStringLiteral("Audio/Source");
}

QHash<QString, PwPortInfo> portsByChannel(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction)
{
  QHash<QString, PwPortInfo> out;
  for (const auto& p : ports) {
    if (p.nodeId != nodeId) {
      continue;
    }
    if (p.direction != direction) {
      continue;
    }
    const QString key = p.audioChannel.isEmpty() ? p.name : p.audioChannel;
    out.insert(key, p);
  }
  return out;
}

QString pickChannelForPort(const PwPortInfo& p)
{
  if (!p.audioChannel.isEmpty()) {
    return p.audioChannel;
  }
  return p.name;
}
} // namespace

EqManager::EqManager(PipeWireThread* pw, PipeWireGraph* graph, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
    , m_graph(graph)
{
  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &EqManager::onGraphChanged);
  }
  scheduleReconcile();
}

void EqManager::refresh()
{
  scheduleReconcile();
}

EqPreset EqManager::presetForNodeName(const QString& nodeName) const
{
  return loadPreset(nodeName);
}

void EqManager::setPresetForNodeName(const QString& nodeName, const EqPreset& preset)
{
  savePreset(nodeName, preset);
  scheduleReconcile();
}

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

QVector<QString> EqManager::targetChannels(const PwNodeInfo& node) const
{
  QVector<QString> channels;
  if (!m_graph) {
    return channels;
  }

  const QList<PwPortInfo> ports = m_graph->ports();
  const QString dir = node.mediaClass == QStringLiteral("Audio/Sink") ? QStringLiteral("in") : QStringLiteral("out");
  for (const auto& p : ports) {
    if (p.nodeId != node.id || p.direction != dir) {
      continue;
    }
    const QString ch = pickChannelForPort(p);
    if (!channels.contains(ch)) {
      channels.push_back(ch);
    }
  }

  if (channels.isEmpty()) {
    channels = {QStringLiteral("FL"), QStringLiteral("FR")};
  }
  return channels;
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

    QVector<QString> chNames = targetChannels(node);
    QVector<ParametricEqFilter::Channel> channels;
    channels.reserve(chNames.size());
    for (const auto& ch : chNames) {
      ParametricEqFilter::Channel c;
      c.name = ch;
      channels.push_back(c);
    }

    QString filterNodeName = QStringLiteral("headroom.eq.%1").arg(node.name);
    filterNodeName.replace('/', '_');
    filterNodeName.replace(' ', '_');
    QString filterDesc = tr("EQ — %1").arg(nodeLabel(node));

    eq.filter = new ParametricEqFilter(m_pw, filterNodeName, filterDesc, channels, this);
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

  const bool isSink = eq.targetMediaClass == QStringLiteral("Audio/Sink");

  const QString targetDir = isSink ? QStringLiteral("in") : QStringLiteral("out");
  const QHash<QString, PwPortInfo> targetPorts = portsByChannel(ports, targetId, targetDir);
  const QHash<QString, PwPortInfo> filterIn = portsByChannel(ports, filterId, QStringLiteral("in"));
  const QHash<QString, PwPortInfo> filterOut = portsByChannel(ports, filterId, QStringLiteral("out"));

  if (targetPorts.isEmpty() || filterIn.isEmpty() || filterOut.isEmpty()) {
    return;
  }

  if (isSink) {
    // Ensure EQ output -> sink.
    for (const auto& sinkPort : targetPorts) {
      const QString ch = pickChannelForPort(sinkPort);
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
      const QString ch = pickChannelForPort(srcPort);
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
