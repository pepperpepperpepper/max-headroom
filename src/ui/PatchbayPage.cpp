#include "PatchbayPage.h"

#include "backend/PatchbayProfileHooks.h"
#include "backend/PatchbayProfiles.h"
#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"
#include "ui/PatchbayProfileHooksDialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSettings>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

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
    connect(m_graph, &PipeWireGraph::topologyChanged, this, &PatchbayPage::scheduleRebuild);
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
  const QString name =
      QInputDialog::getText(this, tr("Save Patchbay Profile"), tr("Profile name:"), QLineEdit::Normal, initial, &ok).trimmed();
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

  const auto choice =
      QMessageBox::question(this, tr("Delete Profile"), tr("Delete profile “%1”?").arg(name), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
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
