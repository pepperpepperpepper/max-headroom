#pragma once

#include <QObject>
#include <QElapsedTimer>

class PipeWireGraph;
class QTimer;

class PatchbayPersistentProfileController final : public QObject
{
  Q_OBJECT

public:
  explicit PatchbayPersistentProfileController(PipeWireGraph* graph, QObject* parent = nullptr);

public slots:
  void applyNow();

private:
  void scheduleApply();
  void apply();

  PipeWireGraph* m_graph = nullptr;
  QTimer* m_timer = nullptr;
  bool m_applying = false;
  QElapsedTimer m_clock;
  qint64 m_cooldownUntilMs = 0;
};
