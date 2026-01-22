#pragma once

#include <QObject>

class PipeWireGraph;
class QTimer;

class PatchbayAutoConnectController final : public QObject
{
  Q_OBJECT

public:
  explicit PatchbayAutoConnectController(PipeWireGraph* graph, QObject* parent = nullptr);

public slots:
  void applyNow();

private:
  void scheduleApply();
  void apply();

  PipeWireGraph* m_graph = nullptr;
  QTimer* m_timer = nullptr;
  bool m_applying = false;
};

