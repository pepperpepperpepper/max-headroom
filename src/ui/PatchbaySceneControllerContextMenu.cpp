#include "PatchbayPage.h"

#include "PatchbaySceneInternal.h"

#include "backend/PatchbayPortConfig.h"

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTransform>

using namespace patchbayui;

bool PatchbayPage::handleSceneContextMenu(QGraphicsSceneContextMenuEvent* e)
{
  if (!m_scene || !e) {
    return false;
  }

  const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
  QGraphicsItem* portItem = nullptr;
  quint32 linkId = 0;
  for (QGraphicsItem* item : hitItems) {
    if (!item) {
      continue;
    }
    if (item->data(kDataPortId).isValid()) {
      portItem = item;
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

  if (portItem) {
    const QString nodeName = portItem->data(kDataNodeName).toString();
    const QString portName = portItem->data(kDataPortName).toString();
    const bool locked = portItem->data(kDataPortLocked).toBool();

    QSettings s;
    const auto existingAlias = PatchbayPortConfigStore::customAlias(s, nodeName, portName);

    QMenu menu;
    QAction* setAlias = menu.addAction(tr("Set Alias…"));
    QAction* clearAlias = menu.addAction(tr("Clear Alias"));
    clearAlias->setEnabled(existingAlias.has_value());
    menu.addSeparator();
    QAction* toggleLock = menu.addAction(locked ? tr("Unlock Port") : tr("Lock Port"));

    QAction* chosen = menu.exec(e->screenPos());
    if (!chosen) {
      return true;
    }
    if (chosen == setAlias) {
      bool ok = false;
      const QString initial = existingAlias.value_or(QString{});
      const QString alias = QInputDialog::getText(this, tr("Port Alias"), tr("Alias:"), QLineEdit::Normal, initial, &ok).trimmed();
      if (ok) {
        if (alias.isEmpty()) {
          PatchbayPortConfigStore::clearCustomAlias(s, nodeName, portName);
        } else {
          PatchbayPortConfigStore::setCustomAlias(s, nodeName, portName, alias);
        }
        scheduleRebuild();
      }
      return true;
    }
    if (chosen == clearAlias) {
      PatchbayPortConfigStore::clearCustomAlias(s, nodeName, portName);
      scheduleRebuild();
      return true;
    }
    if (chosen == toggleLock) {
      PatchbayPortConfigStore::setLocked(s, nodeName, portName, !locked);
      scheduleRebuild();
      return true;
    }
    return true;
  }

  if (linkId != 0) {
    QMenu menu;
    QAction* disconnect = menu.addAction(tr("Disconnect"));
    QAction* chosen = menu.exec(e->screenPos());
    if (chosen == disconnect) {
      (void)tryDisconnectLink(linkId);
      return true;
    }
    return true;
  }

  return false;
}

