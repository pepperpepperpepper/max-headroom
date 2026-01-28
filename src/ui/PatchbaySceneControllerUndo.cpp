#include "PatchbayPage.h"

#include "PatchbaySceneInternal.h"

#include "backend/PatchbayPortConfig.h"

#include <optional>

#include <QCursor>
#include <QGraphicsItem>
#include <QSettings>
#include <QToolTip>
#include <QUndoCommand>
#include <QUndoStack>

using namespace patchbayui;

namespace {
class PatchbayConnectCommand final : public QUndoCommand
{
public:
  PatchbayConnectCommand(PipeWireGraph* graph,
                         uint32_t outNodeId,
                         uint32_t outPortId,
                         uint32_t inNodeId,
                         uint32_t inPortId,
                         const QString& label)
      : QUndoCommand(label)
      , m_graph(graph)
      , m_outNodeId(outNodeId)
      , m_outPortId(outPortId)
      , m_inNodeId(inNodeId)
      , m_inPortId(inPortId)
  {
  }

  void redo() override
  {
    if (!m_graph) {
      return;
    }
    if (linkIdByPorts(m_graph->links(), m_outPortId, m_inPortId).has_value()) {
      return;
    }
    (void)m_graph->createLink(m_outNodeId, m_outPortId, m_inNodeId, m_inPortId);
  }

  void undo() override
  {
    if (!m_graph) {
      return;
    }
    const auto id = linkIdByPorts(m_graph->links(), m_outPortId, m_inPortId);
    if (!id) {
      return;
    }
    (void)m_graph->destroyLink(*id);
  }

private:
  PipeWireGraph* m_graph = nullptr;
  uint32_t m_outNodeId = 0;
  uint32_t m_outPortId = 0;
  uint32_t m_inNodeId = 0;
  uint32_t m_inPortId = 0;
};

class PatchbayDisconnectCommand final : public QUndoCommand
{
public:
  PatchbayDisconnectCommand(PipeWireGraph* graph,
                            uint32_t outNodeId,
                            uint32_t outPortId,
                            uint32_t inNodeId,
                            uint32_t inPortId,
                            const QString& label)
      : QUndoCommand(label)
      , m_graph(graph)
      , m_outNodeId(outNodeId)
      , m_outPortId(outPortId)
      , m_inNodeId(inNodeId)
      , m_inPortId(inPortId)
  {
  }

  void redo() override
  {
    if (!m_graph) {
      return;
    }
    const auto id = linkIdByPorts(m_graph->links(), m_outPortId, m_inPortId);
    if (!id) {
      return;
    }
    (void)m_graph->destroyLink(*id);
  }

  void undo() override
  {
    if (!m_graph) {
      return;
    }
    if (linkIdByPorts(m_graph->links(), m_outPortId, m_inPortId).has_value()) {
      return;
    }
    (void)m_graph->createLink(m_outNodeId, m_outPortId, m_inNodeId, m_inPortId);
  }

private:
  PipeWireGraph* m_graph = nullptr;
  uint32_t m_outNodeId = 0;
  uint32_t m_outPortId = 0;
  uint32_t m_inNodeId = 0;
  uint32_t m_inPortId = 0;
};
} // namespace

bool PatchbayPage::tryConnectPorts(QGraphicsItem* outPortItem, QGraphicsItem* inPortItem)
{
  if (!m_graph || !m_undo || !outPortItem || !inPortItem) {
    return false;
  }

  const uint32_t outNodeId = outPortItem->data(kDataNodeId).toUInt();
  const uint32_t outPortId = outPortItem->data(kDataPortId).toUInt();
  const uint32_t inNodeId = inPortItem->data(kDataNodeId).toUInt();
  const uint32_t inPortId = inPortItem->data(kDataPortId).toUInt();
  if (outNodeId == 0u || outPortId == 0u || inNodeId == 0u || inPortId == 0u) {
    return false;
  }

  const QString outNodeName = outPortItem->data(kDataNodeName).toString();
  const QString outPortName = outPortItem->data(kDataPortName).toString();
  const QString inNodeName = inPortItem->data(kDataNodeName).toString();
  const QString inPortName = inPortItem->data(kDataPortName).toString();

  QSettings s;
  const bool outLocked = PatchbayPortConfigStore::isLocked(s, outNodeName, outPortName);
  const bool inLocked = PatchbayPortConfigStore::isLocked(s, inNodeName, inPortName);
  if (outLocked || inLocked) {
    QToolTip::showText(QCursor::pos(), tr("Cannot connect: one or more ports are locked."), this);
    return false;
  }

  if (linkIdByPorts(m_graph->links(), outPortId, inPortId).has_value()) {
    QToolTip::showText(QCursor::pos(), tr("Already connected."), this);
    return false;
  }

  const PwNodeInfo outNode = m_graph->nodeById(outNodeId).value_or(PwNodeInfo{});
  const PwNodeInfo inNode = m_graph->nodeById(inNodeId).value_or(PwNodeInfo{});

  const QString outPortLabel = PatchbayPortConfigStore::customAlias(s, outNodeName, outPortName).value_or(outPortName);
  const QString inPortLabel = PatchbayPortConfigStore::customAlias(s, inNodeName, inPortName).value_or(inPortName);

  const QString text = tr("Connect %1:%2 → %3:%4").arg(nodeLabelFor(outNode), outPortLabel, nodeLabelFor(inNode), inPortLabel);
  m_undo->push(new PatchbayConnectCommand(m_graph, outNodeId, outPortId, inNodeId, inPortId, text));
  return true;
}

bool PatchbayPage::tryDisconnectLink(quint32 linkId)
{
  if (!m_graph || !m_undo || linkId == 0) {
    return false;
  }

  std::optional<PwLinkInfo> link;
  for (const auto& l : m_graph->links()) {
    if (l.id == linkId) {
      link = l;
      break;
    }
  }
  if (!link.has_value()) {
    return false;
  }

  const auto outPort = portById(m_graph->ports(), link->outputPortId);
  const auto inPort = portById(m_graph->ports(), link->inputPortId);
  const PwNodeInfo outNode = m_graph->nodeById(link->outputNodeId).value_or(PwNodeInfo{});
  const PwNodeInfo inNode = m_graph->nodeById(link->inputNodeId).value_or(PwNodeInfo{});

  const QString outNodeName = outNode.name;
  const QString inNodeName = inNode.name;
  const QString outPortName = outPort ? outPort->name : QString{};
  const QString inPortName = inPort ? inPort->name : QString{};

  QSettings s;
  const bool outLocked = PatchbayPortConfigStore::isLocked(s, outNodeName, outPortName);
  const bool inLocked = PatchbayPortConfigStore::isLocked(s, inNodeName, inPortName);
  if (outLocked || inLocked) {
    QToolTip::showText(QCursor::pos(), tr("Cannot disconnect: one or more ports are locked."), this);
    return false;
  }

  const QString outPortLabel = PatchbayPortConfigStore::customAlias(s, outNodeName, outPortName).value_or(outPortName);
  const QString inPortLabel = PatchbayPortConfigStore::customAlias(s, inNodeName, inPortName).value_or(inPortName);
  const QString text = tr("Disconnect %1:%2 → %3:%4").arg(nodeLabelFor(outNode), outPortLabel, nodeLabelFor(inNode), inPortLabel);

  m_undo->push(new PatchbayDisconnectCommand(m_graph, link->outputNodeId, link->outputPortId, link->inputNodeId, link->inputPortId, text));
  return true;
}

