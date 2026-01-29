#include "EqManager.h"

#include "backend/PipeWireGraph.h"

EqManager::EqManager(PipeWireThread* pw, PipeWireGraph* graph, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
    , m_graph(graph)
{
  if (m_graph) {
    connect(m_graph, &PipeWireGraph::topologyChanged, this, &EqManager::onGraphChanged);
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
