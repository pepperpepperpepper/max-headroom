#include "GraphPage.h"

#include "backend/PipeWireGraph.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
bool matchesFilter(const QString& haystack, const QString& needle)
{
  if (needle.trimmed().isEmpty()) {
    return true;
  }
  return haystack.contains(needle, Qt::CaseInsensitive);
}
} // namespace

GraphPage::GraphPage(PipeWireGraph* graph, QWidget* parent)
    : QWidget(parent)
    , m_graph(graph)
{
  auto* root = new QVBoxLayout(this);

  auto* top = new QHBoxLayout();
  top->addWidget(new QLabel(tr("Filter:"), this));
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Type to filter nodes/ports/links…"));
  top->addWidget(m_filter, 1);
  root->addLayout(top);

  m_tree = new QTreeWidget(this);
  m_tree->setColumnCount(6);
  m_tree->setHeaderLabels({tr("Kind"), tr("Id"), tr("Node"), tr("Name"), tr("Dir"), tr("Class")});
  m_tree->setUniformRowHeights(true);
  root->addWidget(m_tree, 1);

  connect(m_filter, &QLineEdit::textChanged, this, &GraphPage::rebuild);
  if (m_graph) {
    connect(m_graph, &PipeWireGraph::topologyChanged, this, &GraphPage::rebuild);
  }

  rebuild();
}

void GraphPage::rebuild()
{
  const QString needle = m_filter ? m_filter->text() : QString{};

  m_tree->clear();
  auto* nodesRoot = new QTreeWidgetItem(m_tree, {tr("Nodes")});
  auto* portsRoot = new QTreeWidgetItem(m_tree, {tr("Ports")});
  auto* linksRoot = new QTreeWidgetItem(m_tree, {tr("Links")});

  if (!m_graph) {
    m_tree->expandAll();
    return;
  }

  const auto nodes = m_graph->nodes();
  const auto ports = m_graph->ports();
  const auto links = m_graph->links();

  for (const auto& n : nodes) {
    if (n.name.startsWith(QStringLiteral("headroom.meter.")) || n.name == QStringLiteral("headroom.visualizer") ||
        n.name == QStringLiteral("headroom.recorder")) {
      continue;
    }
    const QString hay = QStringLiteral("%1 %2 %3 %4 %5 %6")
                            .arg(n.id)
                            .arg(n.name, n.description, n.mediaClass, n.appName, n.appProcessBinary);
    if (!matchesFilter(hay, needle)) {
      continue;
    }
    auto* item = new QTreeWidgetItem(nodesRoot, {tr("Node"), QString::number(n.id), QString::number(n.id), n.description, {}, n.mediaClass});
    item->setToolTip(3, n.name);
    item->setToolTip(5, n.objectSerial);
  }

  for (const auto& p : ports) {
    const QString hay = QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
                            .arg(p.id)
                            .arg(p.nodeId)
                            .arg(p.name, p.alias, p.direction, p.audioChannel, p.mediaType);
    if (!matchesFilter(hay, needle)) {
      continue;
    }
    const QString cls = !p.audioChannel.isEmpty() ? p.audioChannel : (!p.mediaType.isEmpty() ? p.mediaType : p.formatDsp);
    auto* item = new QTreeWidgetItem(portsRoot,
                                     {tr("Port"), QString::number(p.id), QString::number(p.nodeId), p.alias, p.direction, cls});
    item->setToolTip(3, p.name);
  }

  for (const auto& l : links) {
    const QString hay = QStringLiteral("%1 %2 %3 %4 %5").arg(l.id).arg(l.outputNodeId).arg(l.outputPortId).arg(l.inputNodeId).arg(l.inputPortId);
    if (!matchesFilter(hay, needle)) {
      continue;
    }
    const QString name = QStringLiteral("%1:%2 → %3:%4")
                             .arg(l.outputNodeId)
                             .arg(l.outputPortId)
                             .arg(l.inputNodeId)
                             .arg(l.inputPortId);
    new QTreeWidgetItem(linksRoot, {tr("Link"), QString::number(l.id), {}, name, {}, {}});
  }

  nodesRoot->setExpanded(true);
  portsRoot->setExpanded(false);
  linksRoot->setExpanded(false);
}
