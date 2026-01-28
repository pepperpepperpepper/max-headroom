#include "PatchbayPage.h"

#include "PatchbaySceneInternal.h"

#include "backend/PatchbayPortConfig.h"
#include "settings/SettingsKeys.h"

#include <algorithm>

#include <QBrush>
#include <QFontMetrics>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QLineEdit>
#include <QPainterPath>
#include <QSettings>

using namespace patchbayui;

std::optional<QPointF> PatchbayPage::loadSavedNodePos(const QString& nodeName) const
{
  QSettings s;
  const QString v = s.value(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName)).toString();
  return parsePoint(v);
}

void PatchbayPage::saveLayoutPositions()
{
  QSettings s;
  for (auto it = m_nodeRootByNodeId.begin(); it != m_nodeRootByNodeId.end(); ++it) {
    QGraphicsItem* item = it.value();
    if (!item) {
      continue;
    }
    const QString nodeName = item->data(kDataNodeName).toString();
    if (nodeName.isEmpty()) {
      continue;
    }
    s.setValue(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName), formatPoint(item->pos()));
  }
}

void PatchbayPage::updateLinkPaths()
{
  if (!m_scene) {
    return;
  }

  for (auto it = m_linkVisualById.begin(); it != m_linkVisualById.end(); ++it) {
    const LinkVisual& link = it.value();
    if (!link.item) {
      continue;
    }

    QGraphicsItem* outRoot = m_nodeRootByNodeId.value(link.outputNodeId, nullptr);
    QGraphicsItem* inRoot = m_nodeRootByNodeId.value(link.inputNodeId, nullptr);
    if (!outRoot || !inRoot) {
      continue;
    }

    const auto outPortsIt = m_portLocalPosByNodeId.find(link.outputNodeId);
    const auto inPortsIt = m_portLocalPosByNodeId.find(link.inputNodeId);
    if (outPortsIt == m_portLocalPosByNodeId.end() || inPortsIt == m_portLocalPosByNodeId.end()) {
      continue;
    }
    if (!outPortsIt->contains(link.outputPortId) || !inPortsIt->contains(link.inputPortId)) {
      continue;
    }

    const QPointF p1 = outRoot->mapToScene(outPortsIt->value(link.outputPortId));
    const QPointF p2 = inRoot->mapToScene(inPortsIt->value(link.inputPortId));
    const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
    QPainterPath path(p1);
    path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);
    link.item->setPath(path);
  }

  m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
}

void PatchbayPage::rebuild()
{
  if (!m_scene) {
    return;
  }
  cancelConnectionDrag();
  clearSelection();
  clearLinkSelection();
  m_scene->clear();
  m_nodeRootByNodeId.clear();
  m_portLocalPosByNodeId.clear();
  m_portDotByPortId.clear();
  m_linkVisualById.clear();
  m_hoverLinkId = 0;

  QSettings settings;
  m_layoutEditMode = settings.value(SettingsKeys::patchbayLayoutEditMode()).toBool();
  if (m_view) {
    m_view->setDragMode(m_layoutEditMode ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
  }

  if (!m_graph) {
    return;
  }

  auto nodes = m_graph->nodes();
  const auto ports = m_graph->ports();
  const auto links = m_graph->links();

  const QString needle = m_filter ? m_filter->text().trimmed() : QString{};
  auto matches = [&](const QString& haystack) {
    if (needle.isEmpty()) {
      return true;
    }
    return haystack.contains(needle, Qt::CaseInsensitive);
  };

  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), isInternalNode), nodes.end());

  QHash<uint32_t, QList<PwPortInfo>> inPorts;
  QHash<uint32_t, QList<PwPortInfo>> outPorts;
  for (const auto& p : ports) {
    if (p.direction == QStringLiteral("in")) {
      inPorts[p.nodeId].push_back(p);
    } else if (p.direction == QStringLiteral("out")) {
      outPorts[p.nodeId].push_back(p);
    }
  }

  // Apply user-configured ordering for sinks (Audio/Sink) first.
  {
    const QStringList order = settings.value(SettingsKeys::sinksOrder()).toStringList();
    QHash<QString, int> indexByName;
    indexByName.reserve(order.size());
    for (int i = 0; i < order.size(); ++i) {
      indexByName.insert(order[i], i);
    }

    auto labelOf = [](const PwNodeInfo& n) { return n.description.isEmpty() ? n.name : n.description; };
    std::sort(nodes.begin(), nodes.end(), [&](const PwNodeInfo& a, const PwNodeInfo& b) {
      const bool aSink = a.mediaClass == QStringLiteral("Audio/Sink");
      const bool bSink = b.mediaClass == QStringLiteral("Audio/Sink");
      if (aSink != bSink) {
        return aSink > bSink;
      }
      if (aSink && bSink) {
        const int ia = indexByName.value(a.name, 1'000'000);
        const int ib = indexByName.value(b.name, 1'000'000);
        if (ia != ib) {
          return ia < ib;
        }
      }
      return labelOf(a).toLower() < labelOf(b).toLower();
    });
  }

  const qreal nodeW = 280;
  const qreal headerH = 34;
  const qreal portH = 18;
  const qreal pad = 10;
  const qreal gapX = 26;
  const qreal gapY = 26;
  const qreal labelMaxW = std::max<qreal>(60.0, (nodeW - 2 * pad - 2 * 8 - 16) / 2.0);

  int col = 0;
  const int cols = 3;
  qreal rowY = 0;
  qreal rowMaxH = 0;

  int displayedNodes = 0;

  for (const auto& n : nodes) {
    QList<PwPortInfo> insAll = inPorts.value(n.id);
    QList<PwPortInfo> outsAll = outPorts.value(n.id);

    auto sortPorts = [&](QList<PwPortInfo>& list) {
      std::sort(list.begin(), list.end(), [&](const PwPortInfo& a, const PwPortInfo& b) {
        const PortKind ka = portKindFor(a, n);
        const PortKind kb = portKindFor(b, n);
        if (ka != kb) {
          return static_cast<int>(ka) < static_cast<int>(kb);
        }
        if (ka == PortKind::Audio) {
          const int ra = audioChannelRank(a.audioChannel);
          const int rb = audioChannelRank(b.audioChannel);
          if (ra != rb) {
            return ra < rb;
          }
        }
        const QString sa = portSortKey(a);
        const QString sb = portSortKey(b);
        if (sa != sb) {
          return sa < sb;
        }
        return a.id < b.id;
      });
    };
    sortPorts(insAll);
    sortPorts(outsAll);

    const bool nodeTextMatches = matches(QStringLiteral("%1 %2 %3 %4 %5")
                                             .arg(n.mediaClass, n.appName, n.appProcessBinary, n.name, n.description));

    QList<PwPortInfo> ins = insAll;
    QList<PwPortInfo> outs = outsAll;
    if (!needle.isEmpty() && !nodeTextMatches) {
      QList<PwPortInfo> insMatched;
      QList<PwPortInfo> outsMatched;
      for (const auto& p : insAll) {
        const QString custom = PatchbayPortConfigStore::customAlias(settings, n.name, p.name).value_or(QString{});
        if (matches(QStringLiteral("%1 %2 %3 %4 %5 %6").arg(p.name, p.alias, custom, p.audioChannel, p.mediaType, p.direction))) {
          insMatched.push_back(p);
        }
      }
      for (const auto& p : outsAll) {
        const QString custom = PatchbayPortConfigStore::customAlias(settings, n.name, p.name).value_or(QString{});
        if (matches(QStringLiteral("%1 %2 %3 %4 %5 %6").arg(p.name, p.alias, custom, p.audioChannel, p.mediaType, p.direction))) {
          outsMatched.push_back(p);
        }
      }
      if (insMatched.isEmpty() && outsMatched.isEmpty()) {
        continue;
      }
      ins = insMatched;
      outs = outsMatched;
    } else if (!needle.isEmpty() && nodeTextMatches) {
      // Node-level match: show all ports.
    }

    const int inCount = ins.size();
    const int outCount = outs.size();
    const int portCount = std::max(inCount, outCount);
    const qreal nodeH = headerH + pad + portH * std::max(1, portCount) + pad;

    ++displayedNodes;

    const qreal x = col * (nodeW + gapX);
    const qreal y = rowY;
    col++;
    rowMaxH = std::max(rowMaxH, nodeH);
    if (col >= cols) {
      col = 0;
      rowY += rowMaxH + gapY;
      rowMaxH = 0;
    }

    QPointF pos(x, y);
    if (const auto saved = loadSavedNodePos(n.name)) {
      pos = *saved;
    }

    auto* root = new QGraphicsRectItem(QRectF(0, 0, nodeW, nodeH));
    root->setPen(Qt::NoPen);
    root->setBrush(QBrush(Qt::transparent));
    root->setPos(pos);
    root->setZValue(1);
    root->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
    root->setData(kDataNodeName, n.name);
    if (m_layoutEditMode) {
      root->setFlag(QGraphicsItem::ItemIsMovable, true);
      root->setFlag(QGraphicsItem::ItemIsSelectable, true);
      root->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
      root->setCursor(Qt::OpenHandCursor);
    }
    m_scene->addItem(root);
    m_nodeRootByNodeId.insert(n.id, root);

    {
      QPainterPath boxPath;
      boxPath.addRoundedRect(QRectF(0, 0, nodeW, nodeH), 12, 12);
      auto* box = new QGraphicsPathItem(boxPath, root);
      box->setPen(QPen(QColor(55, 62, 82), 1));
      box->setBrush(QBrush(QColor(26, 30, 40)));
      box->setZValue(1);
      if (m_layoutEditMode) {
        box->setAcceptedMouseButtons(Qt::NoButton);
      }
    }

    auto* header = new QGraphicsRectItem(QRectF(0, 0, nodeW, headerH), root);
    header->setPen(Qt::NoPen);
    header->setBrush(QBrush(QColor(17, 20, 28)));
    header->setZValue(2);
    if (m_layoutEditMode) {
      header->setAcceptedMouseButtons(Qt::NoButton);
    }

    auto* title = new QGraphicsTextItem(n.description.isEmpty() ? n.name : n.description, root);
    title->setDefaultTextColor(QColor(220, 230, 250));
    title->setPos(pad, 6);
    title->setZValue(3);
    if (m_layoutEditMode) {
      title->setAcceptedMouseButtons(Qt::NoButton);
    }

    // Ports (inputs left, outputs right)
    auto addPort = [&](const PwPortInfo& pinfo, int index, bool isInput) {
      const qreal py = headerH + pad + portH * index;
      const qreal cx = isInput ? pad : (nodeW - pad);
      const QPointF center(cx, py + portH * 0.5);
      m_portLocalPosByNodeId[n.id].insert(pinfo.id, center);

      const QRectF dot(center.x() - 4, center.y() - 4, 8, 8);
      const PortKind kind = portKindFor(pinfo, n);
      const QColor dotColor = isInput ? inColorFor(kind, false) : outColorFor(kind, false);
      const bool locked = PatchbayPortConfigStore::isLocked(settings, n.name, pinfo.name);
      const auto customAlias = PatchbayPortConfigStore::customAlias(settings, n.name, pinfo.name);
      const QString basePw = pinfo.name.isEmpty() ? pinfo.alias : pinfo.name;

      QString defaultDisplayBase = basePw;
      const bool isHeadroomEqNode = n.name.startsWith(QStringLiteral("headroom.eq."));
      if (isHeadroomEqNode) {
        // EQ ports are named like "in_FL"/"out_FL" but also carry PW_KEY_AUDIO_CHANNEL.
        // Prefer showing the channel label (FL/FR/...) or, failing that, strip the prefix.
        if (!pinfo.audioChannel.isEmpty()) {
          defaultDisplayBase = pinfo.audioChannel;
        } else if (defaultDisplayBase.startsWith(QStringLiteral("in_")) || defaultDisplayBase.startsWith(QStringLiteral("out_"))) {
          defaultDisplayBase = defaultDisplayBase.mid(3);
        }
      }

      const QString displayBase = customAlias.value_or(defaultDisplayBase);
      auto* ellipse = new QGraphicsEllipseItem(dot, root);
      if (locked) {
        ellipse->setPen(QPen(QColor(250, 204, 21), 2));
      } else {
        ellipse->setPen(Qt::NoPen);
      }
      ellipse->setBrush(QBrush(dotColor));
      ellipse->setZValue(4);
      m_portDotByPortId.insert(pinfo.id, ellipse);
      ellipse->setData(kDataPortId, QVariant::fromValue(static_cast<quint32>(pinfo.id)));
      ellipse->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
      ellipse->setData(kDataPortDir, isInput ? 1 : 0);
      ellipse->setData(kDataPortKind, static_cast<int>(kind));
      ellipse->setData(kDataNodeName, n.name);
      ellipse->setData(kDataPortName, pinfo.name);
      ellipse->setData(kDataPortLocked, locked);
      const QString dirLabel = isInput ? QStringLiteral("in") : QStringLiteral("out");
      const QString typeLabel =
          (kind == PortKind::Midi) ? QStringLiteral("MIDI") : ((kind == PortKind::Audio) ? QStringLiteral("Audio") : QStringLiteral("Other"));
      QStringList tipLines;
      tipLines << displayBase;
      if (displayBase != basePw && !basePw.isEmpty()) {
        tipLines << QStringLiteral("port: %1").arg(basePw);
      }
      if (!pinfo.alias.isEmpty() && pinfo.alias != basePw) {
        tipLines << QStringLiteral("pw alias: %1").arg(pinfo.alias);
      }
      if (!pinfo.audioChannel.isEmpty()) {
        tipLines << QStringLiteral("channel: %1").arg(pinfo.audioChannel);
      }
      tipLines << QStringLiteral("%1 (%2)").arg(dirLabel, typeLabel);
      if (locked) {
        tipLines << QStringLiteral("LOCKED");
      }
      ellipse->setToolTip(tipLines.join('\n'));
      if (m_layoutEditMode) {
        ellipse->setAcceptedMouseButtons(Qt::NoButton);
      }

      QString label = displayBase;
      if (isHeadroomEqNode && !customAlias.has_value()) {
        label = QStringLiteral("%1 %2").arg(isInput ? QStringLiteral("in") : QStringLiteral("out"), displayBase);
      }
      if (!pinfo.audioChannel.isEmpty()) {
        const QString ch = pinfo.audioChannel.trimmed();
        const bool redundant = isRedundantChannelLabel(displayBase, ch) || isRedundantChannelLabel(basePw, ch) || isRedundantChannelLabel(pinfo.alias, ch);
        if (!ch.isEmpty() && !redundant) {
          label = QStringLiteral("%1 (%2)").arg(displayBase, ch);
        }
      }

      auto* text = new QGraphicsTextItem(root);
      text->setDefaultTextColor(QColor(170, 180, 200));
      text->setToolTip(ellipse->toolTip());
      text->setData(kDataPortId, QVariant::fromValue(static_cast<quint32>(pinfo.id)));
      text->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
      text->setData(kDataPortDir, isInput ? 1 : 0);
      text->setData(kDataPortKind, static_cast<int>(kind));
      text->setData(kDataNodeName, n.name);
      text->setData(kDataPortName, pinfo.name);
      text->setData(kDataPortLocked, locked);
      const QFontMetrics fm(text->font());
      text->setPlainText(fm.elidedText(label, Qt::ElideRight, static_cast<int>(labelMaxW)));
      const qreal tx = isInput ? (center.x() + 8) : (center.x() - 8 - text->boundingRect().width());
      text->setPos(tx, py + 1);
      text->setZValue(4);
      if (m_layoutEditMode) {
        text->setAcceptedMouseButtons(Qt::NoButton);
      }
    };

    for (int i = 0; i < ins.size(); ++i) {
      addPort(ins[i], i, true);
    }
    for (int i = 0; i < outs.size(); ++i) {
      addPort(outs[i], i, false);
    }
  }

  if (displayedNodes == 0) {
    auto* empty = m_scene->addText(tr("No matching nodes"));
    empty->setDefaultTextColor(QColor(148, 163, 184));
    empty->setPos(20, 20);
  }

  // Links.
  for (const auto& l : links) {
    QGraphicsItem* outRoot = m_nodeRootByNodeId.value(l.outputNodeId, nullptr);
    QGraphicsItem* inRoot = m_nodeRootByNodeId.value(l.inputNodeId, nullptr);
    if (!outRoot || !inRoot) {
      continue;
    }

    const auto outPortsIt = m_portLocalPosByNodeId.find(l.outputNodeId);
    const auto inPortsIt = m_portLocalPosByNodeId.find(l.inputNodeId);
    if (outPortsIt == m_portLocalPosByNodeId.end() || inPortsIt == m_portLocalPosByNodeId.end()) {
      continue;
    }
    if (!outPortsIt->contains(l.outputPortId) || !inPortsIt->contains(l.inputPortId)) {
      continue;
    }

    const QPointF p1 = outRoot->mapToScene(outPortsIt->value(l.outputPortId));
    const QPointF p2 = inRoot->mapToScene(inPortsIt->value(l.inputPortId));
    const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
    QPainterPath path(p1);
    path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);

    auto* item = m_scene->addPath(path, linkPen());
    item->setZValue(0);
    item->setData(kDataLinkId, QVariant::fromValue(static_cast<quint32>(l.id)));
    item->setToolTip(QStringLiteral("Link %1").arg(l.id));

    LinkVisual vis;
    vis.outputNodeId = l.outputNodeId;
    vis.outputPortId = l.outputPortId;
    vis.inputNodeId = l.inputNodeId;
    vis.inputPortId = l.inputPortId;
    vis.item = item;
    m_linkVisualById.insert(l.id, vis);
  }

  m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
}
