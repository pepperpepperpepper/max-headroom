#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include "backend/EqConfig.h"

class PipeWireThread;
class PipeWireGraph;
struct PwNodeInfo;
struct PwLinkInfo;
struct PwPortInfo;

class ParametricEqFilter;

class EqManager final : public QObject
{
  Q_OBJECT

public:
  explicit EqManager(PipeWireThread* pw, PipeWireGraph* graph, QObject* parent = nullptr);

  void refresh();

  EqPreset presetForNodeName(const QString& nodeName) const;
  void setPresetForNodeName(const QString& nodeName, const EqPreset& preset);

private:
  struct SavedLink final {
    uint32_t outputNodeId = 0;
    uint32_t outputPortId = 0;
    uint32_t inputNodeId = 0;
    uint32_t inputPortId = 0;
  };

  struct ActiveEq final {
    QString targetName;
    QString targetMediaClass;
    uint32_t targetId = 0;

    EqPreset preset;

    ParametricEqFilter* filter = nullptr;
    uint32_t filterNodeId = 0;

    QSet<QString> savedLinkKeys;
    QVector<SavedLink> savedLinks;
  };

  void onGraphChanged();
  void scheduleReconcile();
  void reconcileAll();
  void reconcileOne(ActiveEq& eq);
  void deactivate(ActiveEq& eq);

  EqPreset loadPreset(const QString& nodeName) const;
  void savePreset(const QString& nodeName, const EqPreset& preset);
  QString presetKey(const QString& nodeName) const;

  static QString nodeLabel(const PwNodeInfo& node);
  static QString linkKey(uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort);
  static bool linkExists(const QList<PwLinkInfo>& links, uint32_t outNode, uint32_t outPort, uint32_t inNode, uint32_t inPort);

  PipeWireThread* m_pw = nullptr;
  PipeWireGraph* m_graph = nullptr;

  bool m_reconcileScheduled = false;
  QHash<QString, ActiveEq> m_activeByNodeName;
};
