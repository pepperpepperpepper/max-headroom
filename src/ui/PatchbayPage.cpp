#include "PatchbayPage.h"

#include "backend/PipeWireGraph.h"
#include "backend/PatchbayProfiles.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PatchbayPortConfig.h"
#include "settings/SettingsKeys.h"
#include "ui/PatchbayProfileHooksDialog.h"

#include <algorithm>
#include <optional>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QGraphicsRectItem>
#include <QHBoxLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QFontMetrics>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSettings>
#include <QToolTip>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

namespace {
constexpr int kDataPortId = 0;
constexpr int kDataNodeId = 1;
constexpr int kDataPortDir = 2;  // 0=out, 1=in
constexpr int kDataNodeName = 3;
constexpr int kDataPortKind = 4; // 0=audio, 1=midi, 2=other
constexpr int kDataPortName = 5;
constexpr int kDataPortLocked = 6;
constexpr int kDataLinkId = 10;

enum class PortKind {
  Audio = 0,
  Midi = 1,
  Other = 2,
};

bool isInternalNode(const PwNodeInfo& node)
{
  return node.name.startsWith(QStringLiteral("headroom.meter.")) || node.name == QStringLiteral("headroom.visualizer") ||
      node.name == QStringLiteral("headroom.recorder");
}

QColor outColor() { return QColor(99, 102, 241); }
QColor outSelectedColor() { return QColor(225, 231, 255); }
QColor outHoverColor() { return QColor(165, 180, 252); }
QColor inColor() { return QColor(34, 211, 238); }
QColor inHoverColor() { return QColor(103, 232, 249); }
QColor midiOutColor() { return QColor(249, 115, 22); }
QColor midiOutHoverColor() { return QColor(251, 146, 60); }
QColor midiInColor() { return QColor(34, 197, 94); }
QColor midiInHoverColor() { return QColor(74, 222, 128); }
QColor otherOutColor() { return QColor(148, 163, 184); }
QColor otherOutHoverColor() { return QColor(203, 213, 225); }
QColor otherInColor() { return QColor(148, 163, 184); }
QColor otherInHoverColor() { return QColor(203, 213, 225); }
QPen linkPen() { return QPen(QColor(148, 163, 184, 150), 2); }
QPen linkHoverPen() { return QPen(QColor(226, 232, 240, 200), 3); }
QPen linkSelectedPen() { return QPen(QColor(56, 189, 248, 230), 3); }
QPen dragWirePen()
{
  QPen p(QColor(99, 102, 241, 210), 2);
  p.setStyle(Qt::DashLine);
  return p;
}

std::optional<QPointF> parsePoint(const QString& s)
{
  const QStringList parts = s.split(',', Qt::SkipEmptyParts);
  if (parts.size() != 2) {
    return std::nullopt;
  }
  bool okX = false;
  bool okY = false;
  const double x = parts[0].toDouble(&okX);
  const double y = parts[1].toDouble(&okY);
  if (!okX || !okY) {
    return std::nullopt;
  }
  return QPointF(x, y);
}

QString formatPoint(const QPointF& p)
{
  return QStringLiteral("%1,%2").arg(qRound(p.x())).arg(qRound(p.y()));
}

bool isRedundantChannelLabel(const QString& label, const QString& ch)
{
  if (label.trimmed().isEmpty() || ch.trimmed().isEmpty()) {
    return true;
  }

  const QString l = label.trimmed().toLower();
  const QString c = ch.trimmed().toLower();
  if (l == c) {
    return true;
  }

  return l.endsWith(QStringLiteral("_%1").arg(c)) || l.endsWith(QStringLiteral("-%1").arg(c)) || l.endsWith(QStringLiteral(" %1").arg(c));
}

PortKind portKindFor(const PwPortInfo& p, const PwNodeInfo& node)
{
  const QString mt = p.mediaType.trimmed().toLower();
  if (mt == QStringLiteral("midi")) {
    return PortKind::Midi;
  }
  if (mt == QStringLiteral("audio")) {
    return PortKind::Audio;
  }
  if (!p.audioChannel.isEmpty()) {
    return PortKind::Audio;
  }
  if (p.formatDsp.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (p.formatDsp.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  if (node.mediaClass.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (node.mediaClass.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  if (p.name.contains(QStringLiteral("midi"), Qt::CaseInsensitive) || p.alias.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  return PortKind::Other;
}

QColor outColorFor(PortKind kind, bool hover)
{
  switch (kind) {
    case PortKind::Audio:
      return hover ? outHoverColor() : outColor();
    case PortKind::Midi:
      return hover ? midiOutHoverColor() : midiOutColor();
    case PortKind::Other:
      return hover ? otherOutHoverColor() : otherOutColor();
  }
  return hover ? outHoverColor() : outColor();
}

QColor inColorFor(PortKind kind, bool hover)
{
  switch (kind) {
    case PortKind::Audio:
      return hover ? inHoverColor() : inColor();
    case PortKind::Midi:
      return hover ? midiInHoverColor() : midiInColor();
    case PortKind::Other:
      return hover ? otherInHoverColor() : otherInColor();
  }
  return hover ? inHoverColor() : inColor();
}

std::optional<PwPortInfo> portById(const QList<PwPortInfo>& ports, uint32_t portId)
{
  for (const auto& p : ports) {
    if (p.id == portId) {
      return p;
    }
  }
  return std::nullopt;
}

QString nodeLabelFor(const PwNodeInfo& n)
{
  if (!n.description.isEmpty()) {
    return n.description;
  }
  if (!n.name.isEmpty()) {
    return n.name;
  }
  return QStringLiteral("(unnamed)");
}

int audioChannelRank(const QString& ch)
{
  const QString c = ch.trimmed().toUpper();
  if (c == QStringLiteral("FL")) {
    return 0;
  }
  if (c == QStringLiteral("FR")) {
    return 1;
  }
  if (c == QStringLiteral("FC")) {
    return 2;
  }
  if (c == QStringLiteral("LFE")) {
    return 3;
  }
  if (c == QStringLiteral("RL")) {
    return 4;
  }
  if (c == QStringLiteral("RR")) {
    return 5;
  }
  if (c == QStringLiteral("SL")) {
    return 6;
  }
  if (c == QStringLiteral("SR")) {
    return 7;
  }
  return 1'000'000;
}

QString portSortKey(const PwPortInfo& p)
{
  if (!p.audioChannel.isEmpty()) {
    return p.audioChannel.trimmed().toLower();
  }
  if (!p.name.isEmpty()) {
    return p.name.trimmed().toLower();
  }
  return p.alias.trimmed().toLower();
}

std::optional<uint32_t> linkIdByPorts(const QList<PwLinkInfo>& links, uint32_t outPortId, uint32_t inPortId)
{
  for (const auto& l : links) {
    if (l.outputPortId == outPortId && l.inputPortId == inPortId) {
      return l.id;
    }
  }
  return std::nullopt;
}

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

PatchbayPage::PatchbayPage(PipeWireGraph* graph, QWidget* parent)
    : QWidget(parent)
    , m_graph(graph)
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);

  auto* header = new QWidget(this);
  header->setStyleSheet(QStringLiteral("background-color: #12141a;"));
  auto* headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(10, 8, 10, 8);

  auto* title = new QLabel(tr("Patchbay"), header);
  title->setStyleSheet(QStringLiteral("font-weight: 600; color: #e2e8f0;"));
  headerLayout->addWidget(title);
  headerLayout->addStretch(1);

  auto* profileLabel = new QLabel(tr("Profile:"), header);
  profileLabel->setStyleSheet(QStringLiteral("color: #e2e8f0;"));
  headerLayout->addWidget(profileLabel);

  m_profileCombo = new QComboBox(header);
  m_profileCombo->setMinimumWidth(220);
  m_profileCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  m_profileCombo->setStyleSheet(QStringLiteral(
      "QComboBox { background: #0f172a; color: #e2e8f0; border: 1px solid #334155; border-radius: 6px; padding: 4px 8px; }"
      "QComboBox:focus { border-color: #38bdf8; }"));
  headerLayout->addWidget(m_profileCombo, 0);

  m_profileStrict = new QCheckBox(tr("Strict"), header);
  m_profileStrict->setToolTip(tr("Disconnect other links touching ports in this profile"));
  m_profileStrict->setStyleSheet(QStringLiteral("color: #e2e8f0;"));
  headerLayout->addWidget(m_profileStrict, 0);

  m_profileApply = new QPushButton(tr("Apply"), header);
  headerLayout->addWidget(m_profileApply, 0);

  m_profileSave = new QPushButton(tr("Save…"), header);
  headerLayout->addWidget(m_profileSave, 0);

  m_profileDelete = new QPushButton(tr("Delete"), header);
  headerLayout->addWidget(m_profileDelete, 0);

  m_profileHooks = new QPushButton(tr("Hooks…"), header);
  headerLayout->addWidget(m_profileHooks, 0);

  m_filter = new QLineEdit(header);
  m_filter->setPlaceholderText(tr("Filter nodes/ports…"));
  m_filter->setClearButtonEnabled(true);
  m_filter->setMaximumWidth(360);
  m_filter->setStyleSheet(QStringLiteral(
      "QLineEdit { background: #0f172a; color: #e2e8f0; border: 1px solid #334155; border-radius: 6px; padding: 4px 8px; }"
      "QLineEdit:focus { border-color: #38bdf8; }"));
  headerLayout->addWidget(m_filter, 0);

  root->addWidget(header, 0);

  reloadProfiles();
  if (m_profileCombo) {
    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, [this]() {
      QSettings s;
      s.setValue(SettingsKeys::patchbaySelectedProfileName(), currentProfileName());
      const bool hasProfile = !currentProfileName().isEmpty();
      if (m_profileApply) {
        m_profileApply->setEnabled(hasProfile);
      }
      if (m_profileDelete) {
        m_profileDelete->setEnabled(hasProfile);
      }
      if (m_profileHooks) {
        m_profileHooks->setEnabled(hasProfile);
      }
    });
  }
  if (m_profileApply) {
    connect(m_profileApply, &QPushButton::clicked, this, &PatchbayPage::applySelectedProfile);
  }
  if (m_profileSave) {
    connect(m_profileSave, &QPushButton::clicked, this, &PatchbayPage::saveProfile);
  }
  if (m_profileDelete) {
    connect(m_profileDelete, &QPushButton::clicked, this, &PatchbayPage::deleteSelectedProfile);
  }
  if (m_profileHooks) {
    connect(m_profileHooks, &QPushButton::clicked, this, &PatchbayPage::editProfileHooks);
  }

  m_scene = new QGraphicsScene(this);
  m_scene->setBackgroundBrush(QColor(18, 20, 26));
  m_scene->installEventFilter(this);

  m_view = new QGraphicsView(m_scene, this);
  m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  m_view->setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->setFocusPolicy(Qt::StrongFocus);
  root->addWidget(m_view, 1);

  m_rebuildTimer = new QTimer(this);
  m_rebuildTimer->setSingleShot(true);
  m_rebuildTimer->setInterval(50);
  connect(m_rebuildTimer, &QTimer::timeout, this, &PatchbayPage::rebuild);

  if (m_filter) {
    connect(m_filter, &QLineEdit::textChanged, this, &PatchbayPage::scheduleRebuild);
  }

  auto* findShortcut = new QShortcut(QKeySequence::Find, this);
  findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(findShortcut, &QShortcut::activated, this, [this]() {
    if (!m_filter) {
      return;
    }
    m_filter->setFocus();
    m_filter->selectAll();
  });

  auto* deleteShortcut = new QShortcut(QKeySequence::Delete, m_view);
  deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(deleteShortcut, &QShortcut::activated, this, [this]() {
    if (!m_graph) {
      return;
    }
    const quint32 id = m_selectedLinkId != 0 ? m_selectedLinkId : m_hoverLinkId;
    if (id != 0) {
      (void)tryDisconnectLink(id);
      clearLinkSelection();
    }
  });

  auto* backspaceShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), m_view);
  backspaceShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(backspaceShortcut, &QShortcut::activated, deleteShortcut, &QShortcut::activated);

  auto* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_view);
  escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escapeShortcut, &QShortcut::activated, this, [this]() {
    cancelConnectionDrag();
    clearSelection();
    clearLinkSelection();
    if (m_scene) {
      m_scene->clearSelection();
    }
  });

  m_undo = new QUndoStack(this);
  auto* undoShortcut = new QShortcut(QKeySequence::Undo, m_view);
  undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(undoShortcut, &QShortcut::activated, this, [this]() {
    if (m_undo) {
      m_undo->undo();
    }
  });
  auto* redoShortcut = new QShortcut(QKeySequence::Redo, m_view);
  redoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(redoShortcut, &QShortcut::activated, this, [this]() {
    if (m_undo) {
      m_undo->redo();
    }
  });

  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &PatchbayPage::scheduleRebuild);
  }
  rebuild();
}

void PatchbayPage::refresh()
{
  reloadProfiles();
  scheduleRebuild();
}

void PatchbayPage::scheduleRebuild()
{
  if (m_rebuildTimer) {
    m_rebuildTimer->start();
    return;
  }
  rebuild();
}

void PatchbayPage::reloadProfiles()
{
  if (!m_profileCombo) {
    return;
  }

  const QString current = currentProfileName();
  QSettings s;
  const QString saved = s.value(SettingsKeys::patchbaySelectedProfileName()).toString();

  m_profileCombo->blockSignals(true);
  m_profileCombo->clear();

  m_profileCombo->addItem(tr("(none)"), QString{});

  const QStringList names = PatchbayProfileStore::listProfileNames(s);
  for (const auto& name : names) {
    m_profileCombo->addItem(name, name);
  }

  QString want = !current.isEmpty() ? current : saved;
  int idx = -1;
  for (int i = 0; i < m_profileCombo->count(); ++i) {
    if (m_profileCombo->itemData(i).toString() == want) {
      idx = i;
      break;
    }
  }
  m_profileCombo->setCurrentIndex(idx >= 0 ? idx : 0);
  m_profileCombo->blockSignals(false);

  const bool hasProfile = !currentProfileName().isEmpty();
  if (m_profileApply) {
    m_profileApply->setEnabled(hasProfile);
  }
  if (m_profileDelete) {
    m_profileDelete->setEnabled(hasProfile);
  }
  if (m_profileHooks) {
    m_profileHooks->setEnabled(hasProfile);
  }
}

QString PatchbayPage::currentProfileName() const
{
  if (!m_profileCombo) {
    return {};
  }
  return m_profileCombo->currentData().toString().trimmed();
}

void PatchbayPage::editProfileHooks()
{
  const QString name = currentProfileName();
  if (name.isEmpty()) {
    return;
  }
  PatchbayProfileHooksDialog dlg(name, this);
  dlg.exec();
}

void PatchbayPage::applySelectedProfile()
{
  if (!m_graph) {
    return;
  }

  const QString name = currentProfileName();
  if (name.isEmpty()) {
    return;
  }

  QSettings s;
  const QString prevActive = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();
  const auto profile = PatchbayProfileStore::load(s, name);
  if (!profile) {
    QMessageBox::warning(this, tr("Patchbay Profiles"), tr("Profile “%1” could not be loaded.").arg(name));
    reloadProfiles();
    return;
  }

  PatchbayProfileHookStartResult unloadHook;
  if (!prevActive.isEmpty() && prevActive != name) {
    const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, prevActive);
    unloadHook = startPatchbayProfileHookDetached(prevActive, QString{}, name, PatchbayProfileHookEvent::Unload, h.onUnloadCommand);
  }

  const bool strict = m_profileStrict ? m_profileStrict->isChecked() : false;
  const PatchbayProfileApplyResult r = applyPatchbayProfile(*m_graph, *profile, strict);

  const PatchbayProfileHooks newHooks = PatchbayProfileHooksStore::load(s, name);
  const PatchbayProfileHookStartResult loadHook =
      startPatchbayProfileHookDetached(name, prevActive, QString{}, PatchbayProfileHookEvent::Load, newHooks.onLoadCommand);

  s.setValue(SettingsKeys::patchbayActiveProfileName(), name);

  QString summary = tr("Applied profile “%1”.\nCreated: %2\nAlready present: %3")
                        .arg(profile->name)
                        .arg(r.createdLinks)
                        .arg(r.alreadyPresentLinks);
  if (strict) {
    summary += tr("\nDisconnected: %1").arg(r.disconnectedLinks);
  }
  if (r.missingEndpoints > 0) {
    summary += tr("\nMissing endpoints: %1").arg(r.missingEndpoints);
  }
  if (!r.errors.isEmpty()) {
    summary += tr("\nErrors: %1").arg(r.errors.size());
  }

  QMessageBox box(QMessageBox::Information, tr("Patchbay Profiles"), summary, QMessageBox::Ok, this);

  QString details;
  if (unloadHook.started || !unloadHook.error.isEmpty() || loadHook.started || !loadHook.error.isEmpty()) {
    details += tr("Hooks:\n");
    if (unloadHook.started) {
      details += tr("  - Unload “%1”: started (pid %2)\n").arg(prevActive).arg(unloadHook.pid);
    } else if (!unloadHook.error.isEmpty()) {
      details += tr("  - Unload “%1”: %2\n").arg(prevActive, unloadHook.error);
    }
    if (loadHook.started) {
      details += tr("  - Load “%1”: started (pid %2)\n").arg(name).arg(loadHook.pid);
    } else if (!loadHook.error.isEmpty()) {
      details += tr("  - Load “%1”: %2\n").arg(name, loadHook.error);
    }
    details += "\n";
  }
  if (!r.missing.isEmpty()) {
    details += tr("Missing endpoints:\n");
    const int limit = std::min<int>(r.missing.size(), 40);
    for (int i = 0; i < limit; ++i) {
      details += QStringLiteral("  - %1\n").arg(r.missing[i]);
    }
    if (r.missing.size() > limit) {
      details += tr("  …and %1 more\n").arg(r.missing.size() - limit);
    }
    details += "\n";
  }
  if (!r.errors.isEmpty()) {
    details += tr("Errors:\n");
    for (const auto& e : r.errors) {
      details += QStringLiteral("  - %1\n").arg(e);
    }
  }
  if (!details.trimmed().isEmpty()) {
    box.setDetailedText(details);
  }
  box.exec();
}

void PatchbayPage::saveProfile()
{
  if (!m_graph) {
    return;
  }

  const QString initial = currentProfileName();
  bool ok = false;
  const QString name = QInputDialog::getText(
      this,
      tr("Save Patchbay Profile"),
      tr("Profile name:"),
      QLineEdit::Normal,
      initial,
      &ok).trimmed();
  if (!ok || name.isEmpty()) {
    return;
  }

  QSettings s;
  const bool exists = PatchbayProfileStore::load(s, name).has_value();
  if (exists) {
    const auto choice = QMessageBox::question(
        this,
        tr("Overwrite Profile"),
        tr("Overwrite existing profile “%1”?").arg(name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice != QMessageBox::Yes) {
      return;
    }
  }

  PatchbayProfile p = snapshotPatchbayProfile(name, *m_graph);
  PatchbayProfileStore::save(s, p);

  reloadProfiles();
  if (m_profileCombo) {
    const int idx = m_profileCombo->findData(name);
    if (idx >= 0) {
      m_profileCombo->setCurrentIndex(idx);
    }
  }
}

void PatchbayPage::deleteSelectedProfile()
{
  const QString name = currentProfileName();
  if (name.isEmpty()) {
    return;
  }

  const auto choice = QMessageBox::question(
      this,
      tr("Delete Profile"),
      tr("Delete profile “%1”?").arg(name),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
  if (choice != QMessageBox::Yes) {
    return;
  }

  QSettings s;
  PatchbayProfileStore::remove(s, name);
  PatchbayProfileHooksStore::clear(s, name);
  s.remove(SettingsKeys::patchbaySelectedProfileName());
  if (s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed() == name) {
    s.remove(SettingsKeys::patchbayActiveProfileName());
  }
  reloadProfiles();
}

bool PatchbayPage::eventFilter(QObject* obj, QEvent* event)
{
  if (obj != m_scene || !m_scene) {
    return QWidget::eventFilter(obj, event);
  }

  if (event->type() == QEvent::GraphicsSceneMousePress) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (e->button() != Qt::LeftButton) {
      return QWidget::eventFilter(obj, event);
    }

    if (m_layoutEditMode) {
      QGraphicsItem* item = m_scene->itemAt(e->scenePos(), QTransform());
      if (!item) {
        m_scene->clearSelection();
        clearSelection();
        clearLinkSelection();
      }
      return QWidget::eventFilter(obj, event);
    }

    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    quint32 hitLinkId = 0;
    QGraphicsItem* hitPortItem = nullptr;
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        hitPortItem = item;
        break;
      }
      if (item->data(kDataLinkId).isValid()) {
        hitLinkId = item->data(kDataLinkId).toUInt();
        break;
      }
      if (item->data(kDataNodeId).isValid() || item->data(kDataNodeName).isValid()) {
        break;
      }
    }

    if (!hitPortItem && hitLinkId == 0) {
      cancelConnectionDrag();
      clearSelection();
      clearLinkSelection();
      return QWidget::eventFilter(obj, event);
    }

    if (!hitPortItem && hitLinkId != 0) {
      cancelConnectionDrag();
      clearSelection();
      setSelectedLinkId(hitLinkId);
      return true;
    }

    const quint32 portId = hitPortItem->data(kDataPortId).toUInt();
    const quint32 nodeId = hitPortItem->data(kDataNodeId).toUInt();
    const bool isInput = hitPortItem->data(kDataPortDir).toInt() == 1;
    const int kind = hitPortItem->data(kDataPortKind).toInt();

    clearLinkSelection();
    cancelConnectionDrag();

    if (!isInput) {
      if (hitPortItem->data(kDataPortLocked).toBool()) {
        QToolTip::showText(e->screenPos(), tr("This port is locked."), this);
        clearSelection();
        return true;
      }
      clearSelection();
      m_selectedOutPortId = portId;
      m_selectedOutNodeId = nodeId;
      m_selectedOutPortKind = kind;
      auto* dot = qgraphicsitem_cast<QGraphicsEllipseItem*>(hitPortItem);
      if (!dot) {
        dot = m_portDotByPortId.value(portId, nullptr);
      }
      m_selectedOutDot = dot;
      updatePortDotStyle(m_selectedOutDot);
      beginConnectionDrag();
      updateConnectionDrag(e->scenePos());
      return true;
    }

    if (m_selectedOutPortId != 0 && m_graph && kind == m_selectedOutPortKind) {
      (void)tryConnectPorts(m_selectedOutDot, hitPortItem);
      clearSelection();
      return true;
    }
  } else if (event->type() == QEvent::GraphicsSceneMouseMove) {
    if (m_layoutEditMode) {
      updateLinkPaths();
      return QWidget::eventFilter(obj, event);
    }

    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (m_connectionDragActive) {
      updateConnectionDrag(e->scenePos());
      return true;
    }

    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    QGraphicsEllipseItem* portDot = nullptr;
    quint32 linkId = 0;
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        portDot = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
        if (!portDot) {
          const quint32 portId = item->data(kDataPortId).toUInt();
          portDot = m_portDotByPortId.value(portId, nullptr);
        }
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
    setHoverPortDot(portDot);
    setHoverLinkId(portDot ? 0 : linkId);
  } else if (event->type() == QEvent::GraphicsSceneMouseRelease) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (m_layoutEditMode && e->button() == Qt::LeftButton) {
      updateLinkPaths();
      saveLayoutPositions();
      return QWidget::eventFilter(obj, event);
    }

    if (e->button() == Qt::LeftButton && m_connectionDragActive) {
      endConnectionDrag(e->scenePos());
      return true;
    }
  } else if (event->type() == QEvent::GraphicsSceneContextMenu) {
    auto* e = static_cast<QGraphicsSceneContextMenuEvent*>(event);
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
    return QWidget::eventFilter(obj, event);
  } else if (event->type() == QEvent::GraphicsSceneMouseDoubleClick) {
    auto* e = static_cast<QGraphicsSceneMouseEvent*>(event);
    if (e->button() != Qt::LeftButton) {
      return QWidget::eventFilter(obj, event);
    }
    const auto hitItems = m_scene->items(e->scenePos(), Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
    for (QGraphicsItem* item : hitItems) {
      if (!item) {
        continue;
      }
      if (item->data(kDataPortId).isValid()) {
        return QWidget::eventFilter(obj, event);
      }
      if (item->data(kDataLinkId).isValid() && m_graph) {
        const quint32 linkId = item->data(kDataLinkId).toUInt();
        (void)tryDisconnectLink(linkId);
        clearLinkSelection();
        return true;
      }
      if (item->data(kDataNodeId).isValid() || item->data(kDataNodeName).isValid()) {
        return QWidget::eventFilter(obj, event);
      }
    }
  }

  return QWidget::eventFilter(obj, event);
}

void PatchbayPage::clearSelection()
{
  cancelConnectionDrag();
  if (m_selectedOutDot && m_selectedOutDot->scene() == m_scene) {
    updatePortDotStyle(m_selectedOutDot);
  }
  m_selectedOutNodeId = 0;
  m_selectedOutPortId = 0;
  m_selectedOutPortKind = -1;
  QGraphicsEllipseItem* prev = m_selectedOutDot;
  m_selectedOutDot = nullptr;
  updatePortDotStyle(prev);
}

void PatchbayPage::clearLinkSelection()
{
  m_selectedLinkId = 0;
  updateLinkStyles();
}

void PatchbayPage::updatePortDotStyle(QGraphicsEllipseItem* dot)
{
  if (!dot || dot->scene() != m_scene) {
    return;
  }
  const bool isInput = dot->data(kDataPortDir).toInt() == 1;
  const PortKind kind = static_cast<PortKind>(dot->data(kDataPortKind).toInt());
  if (isInput) {
    dot->setBrush(QBrush(inColorFor(kind, dot == m_hoverPortDot)));
    return;
  }
  if (dot == m_selectedOutDot) {
    dot->setBrush(QBrush(outSelectedColor()));
    return;
  }
  dot->setBrush(QBrush(outColorFor(kind, dot == m_hoverPortDot)));
}

void PatchbayPage::setHoverPortDot(QGraphicsEllipseItem* dot)
{
  if (dot == m_hoverPortDot) {
    return;
  }
  QGraphicsEllipseItem* prev = m_hoverPortDot;
  m_hoverPortDot = dot;
  updatePortDotStyle(prev);
  updatePortDotStyle(m_hoverPortDot);
  updatePortDotStyle(m_selectedOutDot);
}

void PatchbayPage::setSelectedLinkId(quint32 linkId)
{
  if (m_selectedLinkId == linkId) {
    return;
  }
  m_selectedLinkId = linkId;
  updateLinkStyles();
}

void PatchbayPage::setHoverLinkId(quint32 linkId)
{
  if (m_hoverLinkId == linkId) {
    return;
  }
  m_hoverLinkId = linkId;
  updateLinkStyles();
}

void PatchbayPage::updateLinkStyles()
{
  if (!m_scene) {
    return;
  }

  for (auto it = m_linkVisualById.begin(); it != m_linkVisualById.end(); ++it) {
    const quint32 linkId = it.key();
    LinkVisual& link = it.value();
    if (!link.item) {
      continue;
    }
    if (linkId == m_selectedLinkId) {
      link.item->setPen(linkSelectedPen());
      link.item->setZValue(0.6);
    } else if (linkId == m_hoverLinkId) {
      link.item->setPen(linkHoverPen());
      link.item->setZValue(0.5);
    } else {
      link.item->setPen(linkPen());
      link.item->setZValue(0);
    }
  }
}

void PatchbayPage::beginConnectionDrag()
{
  if (!m_scene || !m_selectedOutDot) {
    return;
  }
  if (m_connectionDragItem) {
    delete m_connectionDragItem;
    m_connectionDragItem = nullptr;
  }
  m_connectionDragActive = true;
  const QPointF p1 = m_selectedOutDot->mapToScene(m_selectedOutDot->rect().center());
  QPainterPath path(p1);
  path.lineTo(p1);
  m_connectionDragItem = m_scene->addPath(path, dragWirePen());
  if (m_connectionDragItem) {
    m_connectionDragItem->setZValue(0.55);
  }
}

void PatchbayPage::updateConnectionDrag(const QPointF& scenePos)
{
  if (!m_connectionDragActive || !m_selectedOutDot || !m_connectionDragItem || !m_scene) {
    return;
  }

  const auto hitItems = m_scene->items(scenePos, Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
  QGraphicsEllipseItem* inputDot = nullptr;
  for (QGraphicsItem* item : hitItems) {
    if (item && item->data(kDataPortId).isValid() && item->data(kDataPortDir).toInt() == 1 &&
        item->data(kDataPortKind).toInt() == m_selectedOutPortKind) {
      inputDot = qgraphicsitem_cast<QGraphicsEllipseItem*>(item);
      if (!inputDot) {
        const quint32 portId = item->data(kDataPortId).toUInt();
        inputDot = m_portDotByPortId.value(portId, nullptr);
      }
      if (inputDot) {
        break;
      }
    }
  }
  setHoverPortDot(inputDot);

  const QPointF p1 = m_selectedOutDot->mapToScene(m_selectedOutDot->rect().center());
  QPointF p2 = scenePos;
  if (inputDot) {
    p2 = inputDot->mapToScene(inputDot->rect().center());
  }
  const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
  QPainterPath path(p1);
  path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);
  m_connectionDragItem->setPath(path);
}

void PatchbayPage::endConnectionDrag(const QPointF& scenePos)
{
  if (!m_connectionDragActive) {
    return;
  }

  const auto hitItems = m_scene->items(scenePos, Qt::IntersectsItemShape, Qt::DescendingOrder, QTransform());
  QGraphicsItem* inputItem = nullptr;
  for (QGraphicsItem* item : hitItems) {
    if (item && item->data(kDataPortId).isValid() && item->data(kDataPortDir).toInt() == 1 &&
        item->data(kDataPortKind).toInt() == m_selectedOutPortKind) {
      inputItem = item;
      break;
    }
  }

  const quint32 inputPortId = inputItem ? inputItem->data(kDataPortId).toUInt() : 0;

  cancelConnectionDrag();

  if (inputPortId != 0 && m_selectedOutPortId != 0 && m_graph) {
    (void)tryConnectPorts(m_selectedOutDot, inputItem);
    clearSelection();
  }
}

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

void PatchbayPage::cancelConnectionDrag()
{
  m_connectionDragActive = false;
  if (m_connectionDragItem) {
    delete m_connectionDragItem;
    m_connectionDragItem = nullptr;
  }
  setHoverPortDot(nullptr);
  setHoverLinkId(0);
  updateLinkStyles();
}

std::optional<QPointF> PatchbayPage::loadSavedNodePos(const QString& nodeName) const
{
  QSettings s;
  const QString v = s.value(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName)).toString();
  return parsePoint(v);
}

void PatchbayPage::saveLayoutPositions()
{
  QSettings s;
  for (auto it = m_nodeRootByNodeId.begin(); it != m_nodeRootByNodeId.end(); ++it) {
    QGraphicsItem* item = it.value();
    if (!item) {
      continue;
    }
    const QString nodeName = item->data(kDataNodeName).toString();
    if (nodeName.isEmpty()) {
      continue;
    }
    s.setValue(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName), formatPoint(item->pos()));
  }
}

void PatchbayPage::updateLinkPaths()
{
  if (!m_scene) {
    return;
  }

  for (auto it = m_linkVisualById.begin(); it != m_linkVisualById.end(); ++it) {
    const LinkVisual& link = it.value();
    if (!link.item) {
      continue;
    }

    QGraphicsItem* outRoot = m_nodeRootByNodeId.value(link.outputNodeId, nullptr);
    QGraphicsItem* inRoot = m_nodeRootByNodeId.value(link.inputNodeId, nullptr);
    if (!outRoot || !inRoot) {
      continue;
    }

    const auto outPortsIt = m_portLocalPosByNodeId.find(link.outputNodeId);
    const auto inPortsIt = m_portLocalPosByNodeId.find(link.inputNodeId);
    if (outPortsIt == m_portLocalPosByNodeId.end() || inPortsIt == m_portLocalPosByNodeId.end()) {
      continue;
    }
    if (!outPortsIt->contains(link.outputPortId) || !inPortsIt->contains(link.inputPortId)) {
      continue;
    }

    const QPointF p1 = outRoot->mapToScene(outPortsIt->value(link.outputPortId));
    const QPointF p2 = inRoot->mapToScene(inPortsIt->value(link.inputPortId));
    const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
    QPainterPath path(p1);
    path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);
    link.item->setPath(path);
  }

  m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
}

void PatchbayPage::rebuild()
{
  if (!m_scene) {
    return;
  }
  cancelConnectionDrag();
  clearSelection();
  clearLinkSelection();
  m_scene->clear();
  m_nodeRootByNodeId.clear();
  m_portLocalPosByNodeId.clear();
  m_portDotByPortId.clear();
  m_linkVisualById.clear();
  m_hoverLinkId = 0;

  QSettings settings;
  m_layoutEditMode = settings.value(SettingsKeys::patchbayLayoutEditMode()).toBool();
  if (m_view) {
    m_view->setDragMode(m_layoutEditMode ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
  }

  if (!m_graph) {
    return;
  }

  auto nodes = m_graph->nodes();
  const auto ports = m_graph->ports();
  const auto links = m_graph->links();

  const QString needle = m_filter ? m_filter->text().trimmed() : QString{};
  auto matches = [&](const QString& haystack) {
    if (needle.isEmpty()) {
      return true;
    }
    return haystack.contains(needle, Qt::CaseInsensitive);
  };

  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), isInternalNode), nodes.end());

  QHash<uint32_t, QList<PwPortInfo>> inPorts;
  QHash<uint32_t, QList<PwPortInfo>> outPorts;
  for (const auto& p : ports) {
    if (p.direction == QStringLiteral("in")) {
      inPorts[p.nodeId].push_back(p);
    } else if (p.direction == QStringLiteral("out")) {
      outPorts[p.nodeId].push_back(p);
    }
  }

  // Apply user-configured ordering for sinks (Audio/Sink) first.
  {
    const QStringList order = settings.value(SettingsKeys::sinksOrder()).toStringList();
    QHash<QString, int> indexByName;
    indexByName.reserve(order.size());
    for (int i = 0; i < order.size(); ++i) {
      indexByName.insert(order[i], i);
    }

    auto labelOf = [](const PwNodeInfo& n) { return n.description.isEmpty() ? n.name : n.description; };
    std::sort(nodes.begin(), nodes.end(), [&](const PwNodeInfo& a, const PwNodeInfo& b) {
      const bool aSink = a.mediaClass == QStringLiteral("Audio/Sink");
      const bool bSink = b.mediaClass == QStringLiteral("Audio/Sink");
      if (aSink != bSink) {
        return aSink > bSink;
      }
      if (aSink && bSink) {
        const int ia = indexByName.value(a.name, 1'000'000);
        const int ib = indexByName.value(b.name, 1'000'000);
        if (ia != ib) {
          return ia < ib;
        }
      }
      return labelOf(a).toLower() < labelOf(b).toLower();
    });
  }

  const qreal nodeW = 280;
  const qreal headerH = 34;
  const qreal portH = 18;
  const qreal pad = 10;
  const qreal gapX = 26;
  const qreal gapY = 26;
  const qreal labelMaxW = std::max<qreal>(60.0, (nodeW - 2 * pad - 2 * 8 - 16) / 2.0);

  int col = 0;
  const int cols = 3;
  qreal rowY = 0;
  qreal rowMaxH = 0;

  int displayedNodes = 0;

  for (const auto& n : nodes) {
    QList<PwPortInfo> insAll = inPorts.value(n.id);
    QList<PwPortInfo> outsAll = outPorts.value(n.id);

    auto sortPorts = [&](QList<PwPortInfo>& list) {
      std::sort(list.begin(), list.end(), [&](const PwPortInfo& a, const PwPortInfo& b) {
        const PortKind ka = portKindFor(a, n);
        const PortKind kb = portKindFor(b, n);
        if (ka != kb) {
          return static_cast<int>(ka) < static_cast<int>(kb);
        }
        if (ka == PortKind::Audio) {
          const int ra = audioChannelRank(a.audioChannel);
          const int rb = audioChannelRank(b.audioChannel);
          if (ra != rb) {
            return ra < rb;
          }
        }
        const QString sa = portSortKey(a);
        const QString sb = portSortKey(b);
        if (sa != sb) {
          return sa < sb;
        }
        return a.id < b.id;
      });
    };
    sortPorts(insAll);
    sortPorts(outsAll);

    const bool nodeTextMatches = matches(QStringLiteral("%1 %2 %3 %4 %5")
                                             .arg(n.mediaClass, n.appName, n.appProcessBinary, n.name, n.description));

    QList<PwPortInfo> ins = insAll;
    QList<PwPortInfo> outs = outsAll;
    if (!needle.isEmpty() && !nodeTextMatches) {
      QList<PwPortInfo> insMatched;
      QList<PwPortInfo> outsMatched;
      for (const auto& p : insAll) {
        const QString custom = PatchbayPortConfigStore::customAlias(settings, n.name, p.name).value_or(QString{});
        if (matches(QStringLiteral("%1 %2 %3 %4 %5 %6").arg(p.name, p.alias, custom, p.audioChannel, p.mediaType, p.direction))) {
          insMatched.push_back(p);
        }
      }
      for (const auto& p : outsAll) {
        const QString custom = PatchbayPortConfigStore::customAlias(settings, n.name, p.name).value_or(QString{});
        if (matches(QStringLiteral("%1 %2 %3 %4 %5 %6").arg(p.name, p.alias, custom, p.audioChannel, p.mediaType, p.direction))) {
          outsMatched.push_back(p);
        }
      }
      if (insMatched.isEmpty() && outsMatched.isEmpty()) {
        continue;
      }
      ins = insMatched;
      outs = outsMatched;
    } else if (!needle.isEmpty() && nodeTextMatches) {
      // Node-level match: show all ports.
    }

    const int inCount = ins.size();
    const int outCount = outs.size();
    const int portCount = std::max(inCount, outCount);
    const qreal nodeH = headerH + pad + portH * std::max(1, portCount) + pad;

    ++displayedNodes;

    const qreal x = col * (nodeW + gapX);
    const qreal y = rowY;
    col++;
    rowMaxH = std::max(rowMaxH, nodeH);
    if (col >= cols) {
      col = 0;
      rowY += rowMaxH + gapY;
      rowMaxH = 0;
    }

    QPointF pos(x, y);
    if (const auto saved = loadSavedNodePos(n.name)) {
      pos = *saved;
    }

    auto* root = new QGraphicsRectItem(QRectF(0, 0, nodeW, nodeH));
    root->setPen(Qt::NoPen);
    root->setBrush(QBrush(Qt::transparent));
    root->setPos(pos);
    root->setZValue(1);
    root->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
    root->setData(kDataNodeName, n.name);
    if (m_layoutEditMode) {
      root->setFlag(QGraphicsItem::ItemIsMovable, true);
      root->setFlag(QGraphicsItem::ItemIsSelectable, true);
      root->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
      root->setCursor(Qt::OpenHandCursor);
    }
    m_scene->addItem(root);
    m_nodeRootByNodeId.insert(n.id, root);

    {
      QPainterPath boxPath;
      boxPath.addRoundedRect(QRectF(0, 0, nodeW, nodeH), 12, 12);
      auto* box = new QGraphicsPathItem(boxPath, root);
      box->setPen(QPen(QColor(55, 62, 82), 1));
      box->setBrush(QBrush(QColor(26, 30, 40)));
      box->setZValue(1);
      if (m_layoutEditMode) {
        box->setAcceptedMouseButtons(Qt::NoButton);
      }
    }

    auto* header = new QGraphicsRectItem(QRectF(0, 0, nodeW, headerH), root);
    header->setPen(Qt::NoPen);
    header->setBrush(QBrush(QColor(17, 20, 28)));
    header->setZValue(2);
    if (m_layoutEditMode) {
      header->setAcceptedMouseButtons(Qt::NoButton);
    }

    auto* title = new QGraphicsTextItem(n.description.isEmpty() ? n.name : n.description, root);
    title->setDefaultTextColor(QColor(220, 230, 250));
    title->setPos(pad, 6);
    title->setZValue(3);
    if (m_layoutEditMode) {
      title->setAcceptedMouseButtons(Qt::NoButton);
    }

    // Ports (inputs left, outputs right)
    auto addPort = [&](const PwPortInfo& pinfo, int index, bool isInput) {
      const qreal py = headerH + pad + portH * index;
      const qreal cx = isInput ? pad : (nodeW - pad);
      const QPointF center(cx, py + portH * 0.5);
      m_portLocalPosByNodeId[n.id].insert(pinfo.id, center);

      const QRectF dot(center.x() - 4, center.y() - 4, 8, 8);
      const PortKind kind = portKindFor(pinfo, n);
      const QColor dotColor = isInput ? inColorFor(kind, false) : outColorFor(kind, false);
      const bool locked = PatchbayPortConfigStore::isLocked(settings, n.name, pinfo.name);
      const auto customAlias = PatchbayPortConfigStore::customAlias(settings, n.name, pinfo.name);
      const QString basePw = pinfo.name.isEmpty() ? pinfo.alias : pinfo.name;

      QString defaultDisplayBase = basePw;
      const bool isHeadroomEqNode = n.name.startsWith(QStringLiteral("headroom.eq."));
      if (isHeadroomEqNode) {
        // EQ ports are named like "in_FL"/"out_FL" but also carry PW_KEY_AUDIO_CHANNEL.
        // Prefer showing the channel label (FL/FR/...) or, failing that, strip the prefix.
        if (!pinfo.audioChannel.isEmpty()) {
          defaultDisplayBase = pinfo.audioChannel;
        } else if (defaultDisplayBase.startsWith(QStringLiteral("in_")) || defaultDisplayBase.startsWith(QStringLiteral("out_"))) {
          defaultDisplayBase = defaultDisplayBase.mid(3);
        }
      }

      const QString displayBase = customAlias.value_or(defaultDisplayBase);
      auto* ellipse = new QGraphicsEllipseItem(dot, root);
      if (locked) {
        ellipse->setPen(QPen(QColor(250, 204, 21), 2));
      } else {
        ellipse->setPen(Qt::NoPen);
      }
      ellipse->setBrush(QBrush(dotColor));
      ellipse->setZValue(4);
      m_portDotByPortId.insert(pinfo.id, ellipse);
      ellipse->setData(kDataPortId, QVariant::fromValue(static_cast<quint32>(pinfo.id)));
      ellipse->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
      ellipse->setData(kDataPortDir, isInput ? 1 : 0);
      ellipse->setData(kDataPortKind, static_cast<int>(kind));
      ellipse->setData(kDataNodeName, n.name);
      ellipse->setData(kDataPortName, pinfo.name);
      ellipse->setData(kDataPortLocked, locked);
      const QString dirLabel = isInput ? QStringLiteral("in") : QStringLiteral("out");
      const QString typeLabel = (kind == PortKind::Midi) ? QStringLiteral("MIDI") : ((kind == PortKind::Audio) ? QStringLiteral("Audio") : QStringLiteral("Other"));
      QStringList tipLines;
      tipLines << displayBase;
      if (displayBase != basePw && !basePw.isEmpty()) {
        tipLines << QStringLiteral("port: %1").arg(basePw);
      }
      if (!pinfo.alias.isEmpty() && pinfo.alias != basePw) {
        tipLines << QStringLiteral("pw alias: %1").arg(pinfo.alias);
      }
      if (!pinfo.audioChannel.isEmpty()) {
        tipLines << QStringLiteral("channel: %1").arg(pinfo.audioChannel);
      }
      tipLines << QStringLiteral("%1 (%2)").arg(dirLabel, typeLabel);
      if (locked) {
        tipLines << QStringLiteral("LOCKED");
      }
      ellipse->setToolTip(tipLines.join('\n'));
      if (m_layoutEditMode) {
        ellipse->setAcceptedMouseButtons(Qt::NoButton);
      }

      QString label = displayBase;
      if (!pinfo.audioChannel.isEmpty()) {
        const QString ch = pinfo.audioChannel.trimmed();
        const bool redundant = isRedundantChannelLabel(displayBase, ch) || isRedundantChannelLabel(basePw, ch) || isRedundantChannelLabel(pinfo.alias, ch);
        if (!ch.isEmpty() && !redundant) {
          label = QStringLiteral("%1 (%2)").arg(displayBase, ch);
        }
      }

      auto* text = new QGraphicsTextItem(root);
      text->setDefaultTextColor(QColor(170, 180, 200));
      text->setToolTip(ellipse->toolTip());
      text->setData(kDataPortId, QVariant::fromValue(static_cast<quint32>(pinfo.id)));
      text->setData(kDataNodeId, QVariant::fromValue(static_cast<quint32>(n.id)));
      text->setData(kDataPortDir, isInput ? 1 : 0);
      text->setData(kDataPortKind, static_cast<int>(kind));
      text->setData(kDataNodeName, n.name);
      text->setData(kDataPortName, pinfo.name);
      text->setData(kDataPortLocked, locked);
      const QFontMetrics fm(text->font());
      text->setPlainText(fm.elidedText(label, Qt::ElideRight, static_cast<int>(labelMaxW)));
      const qreal tx = isInput ? (center.x() + 8) : (center.x() - 8 - text->boundingRect().width());
      text->setPos(tx, py + 1);
      text->setZValue(4);
      if (m_layoutEditMode) {
        text->setAcceptedMouseButtons(Qt::NoButton);
      }
    };

    for (int i = 0; i < ins.size(); ++i) {
      addPort(ins[i], i, true);
    }
    for (int i = 0; i < outs.size(); ++i) {
      addPort(outs[i], i, false);
    }
  }

  if (displayedNodes == 0) {
    auto* empty = m_scene->addText(tr("No matching nodes"));
    empty->setDefaultTextColor(QColor(148, 163, 184));
    empty->setPos(20, 20);
  }

  // Links.
  for (const auto& l : links) {
    QGraphicsItem* outRoot = m_nodeRootByNodeId.value(l.outputNodeId, nullptr);
    QGraphicsItem* inRoot = m_nodeRootByNodeId.value(l.inputNodeId, nullptr);
    if (!outRoot || !inRoot) {
      continue;
    }

    const auto outPortsIt = m_portLocalPosByNodeId.find(l.outputNodeId);
    const auto inPortsIt = m_portLocalPosByNodeId.find(l.inputNodeId);
    if (outPortsIt == m_portLocalPosByNodeId.end() || inPortsIt == m_portLocalPosByNodeId.end()) {
      continue;
    }
    if (!outPortsIt->contains(l.outputPortId) || !inPortsIt->contains(l.inputPortId)) {
      continue;
    }

    const QPointF p1 = outRoot->mapToScene(outPortsIt->value(l.outputPortId));
    const QPointF p2 = inRoot->mapToScene(inPortsIt->value(l.inputPortId));
    const qreal dx = std::max<qreal>(40.0, std::abs(p2.x() - p1.x()) * 0.4);
    QPainterPath path(p1);
    path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);

    auto* item = m_scene->addPath(path, linkPen());
    item->setZValue(0);
    item->setData(kDataLinkId, QVariant::fromValue(static_cast<quint32>(l.id)));
    item->setToolTip(QStringLiteral("Link %1").arg(l.id));

    LinkVisual vis;
    vis.outputNodeId = l.outputNodeId;
    vis.outputPortId = l.outputPortId;
    vis.inputNodeId = l.inputNodeId;
    vis.inputPortId = l.inputPortId;
    vis.item = item;
    m_linkVisualById.insert(l.id, vis);
  }

  m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
}
