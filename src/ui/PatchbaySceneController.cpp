#include "PatchbayPage.h"

#include "PatchbaySceneInternal.h"

#include <algorithm>

#include <QBrush>
#include <QEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainterPath>
#include <QToolTip>
#include <QTransform>

using namespace patchbayui;

bool PatchbayPage::eventFilter(QObject* obj, QEvent* event)
{
  if (obj != m_scene || !m_scene) {
    return QWidget::eventFilter(obj, event);
  }

  if (event->type() == QEvent::GraphicsSceneMousePress) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (e->button() != Qt::LeftButton) {
      return QWidget::eventFilter(obj, event);
    }

    if (m_layoutEditMode) {
      QGraphicsItem* item = m_scene->itemAt(e->scenePos(), QTransform());
      if (!item) {
        m_scene->clearSelection();
        clearSelection();
        clearLinkSelection();
      }
      return QWidget::eventFilter(obj, event);
    }

    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    quint32 hitLinkId = 0;
    QGraphicsItem* hitPortItem = nullptr;
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        hitPortItem = item;
        break;
      }
      if (item->data(kDataLinkId).isValid()) {
        hitLinkId = item->data(kDataLinkId).toUInt();
        break;
      }
      if (item->data(kDataNodeId).isValid() || item->data(kDataNodeName).isValid()) {
        break;
      }
    }

    if (!hitPortItem && hitLinkId == 0) {
      cancelConnectionDrag();
      clearSelection();
      clearLinkSelection();
      return QWidget::eventFilter(obj, event);
    }

    if (!hitPortItem && hitLinkId != 0) {
      cancelConnectionDrag();
      clearSelection();
      setSelectedLinkId(hitLinkId);
      return true;
    }

    const quint32 portId = hitPortItem->data(kDataPortId).toUInt();
    const quint32 nodeId = hitPortItem->data(kDataNodeId).toUInt();
    const bool isInput = hitPortItem->data(kDataPortDir).toInt() == 1;
    const int kind = hitPortItem->data(kDataPortKind).toInt();

    clearLinkSelection();
    cancelConnectionDrag();

    if (!isInput) {
      if (hitPortItem->data(kDataPortLocked).toBool()) {
        QToolTip::showText(e->screenPos(), tr("This port is locked."), this);
        clearSelection();
        return true;
      }
      clearSelection();
      m_selectedOutPortId = portId;
      m_selectedOutNodeId = nodeId;
      m_selectedOutPortKind = kind;
      auto* dot = qgraphicsitem_cast<QGraphicsEllipseItem*>(hitPortItem);
      if (!dot) {
        dot = m_portDotByPortId.value(portId, nullptr);
      }
      m_selectedOutDot = dot;
      updatePortDotStyle(m_selectedOutDot);
      beginConnectionDrag();
      updateConnectionDrag(e->scenePos());
      return true;
    }

    if (m_selectedOutPortId != 0 && m_graph && kind == m_selectedOutPortKind) {
      (void)tryConnectPorts(m_selectedOutDot, hitPortItem);
      clearSelection();
      return true;
    }
  } else if (event->type() == QEvent::GraphicsSceneMouseMove) {
    if (m_layoutEditMode) {
      updateLinkPaths();
      return QWidget::eventFilter(obj, event);
    }

    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (m_connectionDragActive) {
      updateConnectionDrag(e->scenePos());
      return true;
    }

    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    QGraphicsEllipseItem* portDot = nullptr;
    quint32 linkId = 0;
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        portDot = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
        if (!portDot) {
          const quint32 portId = item->data(kDataPortId).toUInt();
          portDot = m_portDotByPortId.value(portId, nullptr);
        }
        break;
      }
      if (item->data(kDataLinkId).isValid()) {
        linkId = item->data(kDataLinkId).toUInt();
        break;
      }
      if (item->data(kDataNodeId).isValid() || item->data(kDataNodeName).isValid()) {
        break;
      }
    }
    setHoverPortDot(portDot);
    setHoverLinkId(portDot ? 0 : linkId);
  } else if (event->type() == QEvent::GraphicsSceneMouseRelease) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (m_layoutEditMode && e->button() == Qt::LeftButton) {
      updateLinkPaths();
      saveLayoutPositions();
      return QWidget::eventFilter(obj, event);
    }

    if (e->button() == Qt::LeftButton && m_connectionDragActive) {
      endConnectionDrag(e->scenePos());
      return true;
    }
  } else if (event->type() == QEvent::GraphicsSceneContextMenu) {
    if (handleSceneContextMenu(static_cast<QGraphicsSceneContextMenuEvent*>(event))) {
      return true;
    }
    return QWidget::eventFilter(obj, event);
  } else if (event->type() == QEvent::GraphicsSceneMouseDoubleClick) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (e->button() != Qt::LeftButton) {
      return QWidget::eventFilter(obj, event);
    }
    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        return QWidget::eventFilter(obj, event);
      }
      if (item->data(kDataLinkId).isValid() && m_graph) {
        const quint32 linkId = item->data(kDataLinkId).toUInt();
        (void)tryDisconnectLink(linkId);
        clearLinkSelection();
        return true;
      }
      if (item->data(kDataNodeId).isValid() || item->data(kDataNodeName).isValid()) {
        return QWidget::eventFilter(obj, event);
      }
    }
  }

  return QWidget::eventFilter(obj, event);
}

void PatchbayPage::clearSelection()
{
  cancelConnectionDrag();
  if (m_selectedOutDot && m_selectedOutDot->scene() == m_scene) {
    updatePortDotStyle(m_selectedOutDot);
  }
  m_selectedOutNodeId = 0;
  m_selectedOutPortId = 0;
  m_selectedOutPortKind = -1;
  QGraphicsEllipseItem* prev = m_selectedOutDot;
  m_selectedOutDot = nullptr;
  updatePortDotStyle(prev);
}

void PatchbayPage::clearLinkSelection()
{
  m_selectedLinkId = 0;
  updateLinkStyles();
}

void PatchbayPage::updatePortDotStyle(QGraphicsEllipseItem* dot)
{
  if (!dot || dot->scene() != m_scene) {
    return;
  }
  const bool isInput = dot->data(kDataPortDir).toInt() == 1;
  const PortKind kind = static_cast<PortKind>(dot->data(kDataPortKind).toInt());
  if (isInput) {
    dot->setBrush(QBrush(inColorFor(kind, dot == m_hoverPortDot)));
    return;
  }
  if (dot == m_selectedOutDot) {
    dot->setBrush(QBrush(outSelectedColor()));
    return;
  }
  dot->setBrush(QBrush(outColorFor(kind, dot == m_hoverPortDot)));
}

void PatchbayPage::setHoverPortDot(QGraphicsEllipseItem* dot)
{
  if (dot == m_hoverPortDot) {
    return;
  }
  QGraphicsEllipseItem* prev = m_hoverPortDot;
  m_hoverPortDot = dot;
  updatePortDotStyle(prev);
  updatePortDotStyle(m_hoverPortDot);
  updatePortDotStyle(m_selectedOutDot);
}

void PatchbayPage::setSelectedLinkId(quint32 linkId)
{
  if (m_selectedLinkId == linkId) {
    return;
  }
  m_selectedLinkId = linkId;
  updateLinkStyles();
}

void PatchbayPage::setHoverLinkId(quint32 linkId)
{
  if (m_hoverLinkId == linkId) {
    return;
  }
  m_hoverLinkId = linkId;
  updateLinkStyles();
}

void PatchbayPage::updateLinkStyles()
{
  if (!m_scene) {
    return;
  }

  for (auto it = m_linkVisualById.begin(); it != m_linkVisualById.end(); ++it) {
    const quint32 linkId = it.key();
    LinkVisual& link = it.value();
    if (!link.item) {
      continue;
    }
    if (linkId == m_selectedLinkId) {
      link.item->setPen(linkSelectedPen());
      link.item->setZValue(0.6);
    } else if (linkId == m_hoverLinkId) {
      link.item->setPen(linkHoverPen());
      link.item->setZValue(0.5);
    } else {
      link.item->setPen(linkPen());
      link.item->setZValue(0);
    }
  }
}

void PatchbayPage::beginConnectionDrag()
{
  if (!m_scene || !m_selectedOutDot) {
    return;
  }
  if (m_connectionDragItem) {
    delete m_connectionDragItem;
    m_connectionDragItem = nullptr;
  }
  m_connectionDragActive = true;
  const QPointF p1 = m_selectedOutDot->mapToScene(m_selectedOutDot->rect().center());
  QPainterPath path(p1);
  path.lineTo(p1);
  m_connectionDragItem = m_scene->addPath(path, dragWirePen());
  if (m_connectionDragItem) {
    m_connectionDragItem->setZValue(0.55);
  }
}

void PatchbayPage::updateConnectionDrag(const QPointF& scenePos)
{
  if (!m_connectionDragActive || !m_selectedOutDot || !m_connectionDragItem || !m_scene) {
    return;
  }

  const auto hitItems = m_scene->items(scenePos, Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
  QGraphicsEllipseItem* inputDot = nullptr;
  for (QGraphicsItem* item : hitItems) {
    if (item && item->data(kDataPortId).isValid() && item->data(kDataPortDir).toInt() == 1 &&
        item->data(kDataPortKind).toInt() == m_selectedOutPortKind) {
      inputDot = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
      if (!inputDot) {
        const quint32 portId = item->data(kDataPortId).toUInt();
        inputDot = m_portDotByPortId.value(portId, nullptr);
      }
      if (inputDot) {
        break;
      }
    }
  }
  setHoverPortDot(inputDot);

  const QPointF p1 = m_selectedOutDot->mapToScene(m_selectedOutDot->rect().center());
  QPointF p2 = scenePos;
  if (inputDot) {
    p2 = inputDot->mapToScene(inputDot->rect().center());
  }
  const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
  QPainterPath path(p1);
  path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);
  m_connectionDragItem->setPath(path);
}

void PatchbayPage::endConnectionDrag(const QPointF& scenePos)
{
  if (!m_connectionDragActive) {
    return;
  }

  const auto hitItems = m_scene->items(scenePos, Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
  QGraphicsItem* inputItem = nullptr;
  for (QGraphicsItem* item : hitItems) {
    if (item && item->data(kDataPortId).isValid() && item->data(kDataPortDir).toInt() == 1 &&
        item->data(kDataPortKind).toInt() == m_selectedOutPortKind) {
      inputItem = item;
      break;
    }
  }

  const quint32 inputPortId = inputItem ? inputItem->data(kDataPortId).toUInt() : 0;

  cancelConnectionDrag();

  if (inputPortId != 0 && m_selectedOutPortId != 0 && m_graph) {
    (void)tryConnectPorts(m_selectedOutDot, inputItem);
    clearSelection();
  }
}

void PatchbayPage::cancelConnectionDrag()
{
  m_connectionDragActive = false;
  if (m_connectionDragItem) {
    delete m_connectionDragItem;
    m_connectionDragItem = nullptr;
  }
  setHoverPortDot(nullptr);
  setHoverLinkId(0);
  updateLinkStyles();
}
