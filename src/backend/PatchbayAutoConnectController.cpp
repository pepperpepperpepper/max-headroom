#include "PatchbayAutoConnectController.h"

#include "backend/PatchbayAutoConnectRules.h"
#include "backend/PipeWireGraph.h"

#include <QSettings>
#include <QTimer>

PatchbayAutoConnectController::PatchbayAutoConnectController(PipeWireGraph* graph, QObject* parent)
    : QObject(parent)
    , m_graph(graph)
{
  if (!m_graph) {
    return;
  }
  connect(m_graph, &PipeWireGraph::graphChanged, this, &PatchbayAutoConnectController::scheduleApply);

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  connect(m_timer, &QTimer::timeout, this, &PatchbayAutoConnectController::apply);
}

void PatchbayAutoConnectController::applyNow()
{
  apply();
}

void PatchbayAutoConnectController::scheduleApply()
{
  if (!m_timer || m_applying) {
    return;
  }
  m_timer->start(300);
}

void PatchbayAutoConnectController::apply()
{
  if (!m_graph || m_applying) {
    return;
  }

  m_applying = true;
  QSettings s;
  const AutoConnectConfig cfg = loadAutoConnectConfig(s);
  if (cfg.enabled) {
    (void)applyAutoConnectRules(*m_graph, cfg);
  }
  m_applying = false;
}

