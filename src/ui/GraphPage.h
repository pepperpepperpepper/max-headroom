#pragma once

#include <QWidget>

class QLineEdit;
class QTreeWidget;
class PipeWireGraph;

class GraphPage final : public QWidget
{
  Q_OBJECT

public:
  explicit GraphPage(PipeWireGraph* graph, QWidget* parent = nullptr);

private:
  void rebuild();

  PipeWireGraph* m_graph = nullptr;
  QLineEdit* m_filter = nullptr;
  QTreeWidget* m_tree = nullptr;
};
