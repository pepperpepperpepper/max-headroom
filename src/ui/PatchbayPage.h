#pragma once

#include <optional>
#include <QHash>
#include <QPointF>
#include <QWidget>

class QGraphicsScene;
class QGraphicsView;
class QGraphicsEllipseItem;
class QGraphicsItem;
class QGraphicsPathItem;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTimer;
class PipeWireGraph;
class QUndoStack;

class PatchbayPage final : public QWidget
{
  Q_OBJECT

public:
  explicit PatchbayPage(PipeWireGraph* graph, QWidget* parent = nullptr);

public slots:
  void refresh();

protected:
  bool eventFilter(QObject* obj, QEvent* event) override;

private:
  void scheduleRebuild();
  void rebuild();
  void reloadProfiles();
  QString currentProfileName() const;
  void applySelectedProfile();
  void saveProfile();
  void deleteSelectedProfile();
  void editProfileHooks();
  void clearSelection();
  void clearLinkSelection();
  void updatePortDotStyle(QGraphicsEllipseItem* dot);
  void setHoverPortDot(QGraphicsEllipseItem* dot);
  void setSelectedLinkId(quint32 linkId);
  void setHoverLinkId(quint32 linkId);
  void updateLinkStyles();
  void beginConnectionDrag();
  void updateConnectionDrag(const QPointF& scenePos);
  void endConnectionDrag(const QPointF& scenePos);
  void cancelConnectionDrag();
  bool tryConnectPorts(QGraphicsItem* outPortItem, QGraphicsItem* inPortItem);
  bool tryDisconnectLink(quint32 linkId);
  void saveLayoutPositions();
  void updateLinkPaths();
  std::optional<QPointF> loadSavedNodePos(const QString& nodeName) const;

  PipeWireGraph* m_graph = nullptr;
  QGraphicsScene* m_scene = nullptr;
  QGraphicsView* m_view = nullptr;
  QLineEdit* m_filter = nullptr;
  QTimer* m_rebuildTimer = nullptr;

  QComboBox* m_profileCombo = nullptr;
  QCheckBox* m_profileStrict = nullptr;
  QPushButton* m_profileApply = nullptr;
  QPushButton* m_profileSave = nullptr;
  QPushButton* m_profileDelete = nullptr;
  QPushButton* m_profileHooks = nullptr;
  QUndoStack* m_undo = nullptr;

  quint32 m_selectedOutNodeId = 0;
  quint32 m_selectedOutPortId = 0;
  int m_selectedOutPortKind = -1;
  QGraphicsEllipseItem* m_selectedOutDot = nullptr;
  QGraphicsEllipseItem* m_hoverPortDot = nullptr;

  bool m_connectionDragActive = false;
  QGraphicsPathItem* m_connectionDragItem = nullptr;

  bool m_layoutEditMode = false;
  QHash<quint32, QGraphicsItem*> m_nodeRootByNodeId;
  QHash<quint32, QHash<quint32, QPointF>> m_portLocalPosByNodeId;
  QHash<quint32, QGraphicsEllipseItem*> m_portDotByPortId;
  struct LinkVisual final {
    quint32 outputNodeId = 0;
    quint32 outputPortId = 0;
    quint32 inputNodeId = 0;
    quint32 inputPortId = 0;
    QGraphicsPathItem* item = nullptr;
  };
  QHash<quint32, LinkVisual> m_linkVisualById;
  quint32 m_selectedLinkId = 0;
  quint32 m_hoverLinkId = 0;
};
