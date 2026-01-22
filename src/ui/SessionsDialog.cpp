#include "SessionsDialog.h"

#include "backend/PipeWireGraph.h"
#include "backend/SessionSnapshots.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {
QString summaryForApply(const QString& name, const SessionSnapshotApplyResult& r, bool strictLinks, bool strictSettings)
{
  QString summary;
  summary += QObject::tr("Applied snapshot “%1”.\n").arg(name);
  summary += QObject::tr("\nPatchbay:\n");
  summary += QObject::tr("  strict: %1\n").arg(strictLinks ? QObject::tr("yes") : QObject::tr("no"));
  summary += QObject::tr("  desired links: %1\n").arg(r.patchbay.desiredLinks);
  summary += QObject::tr("  created: %1\n").arg(r.patchbay.createdLinks);
  summary += QObject::tr("  already present: %1\n").arg(r.patchbay.alreadyPresentLinks);
  summary += QObject::tr("  disconnected: %1\n").arg(r.patchbay.disconnectedLinks);
  summary += QObject::tr("  missing endpoints: %1\n").arg(r.patchbay.missingEndpoints);

  summary += QObject::tr("\nDefaults:\n");
  if (r.defaultSinkRequested) {
    summary += QObject::tr("  output: %1\n").arg(r.defaultSinkSet ? QObject::tr("set") : QObject::tr("not set"));
  } else {
    summary += QObject::tr("  output: (not in snapshot)\n");
  }
  if (r.defaultSourceRequested) {
    summary += QObject::tr("  input: %1\n").arg(r.defaultSourceSet ? QObject::tr("set") : QObject::tr("not set"));
  } else {
    summary += QObject::tr("  input: (not in snapshot)\n");
  }

  summary += QObject::tr("\nSettings:\n");
  summary += QObject::tr("  strict: %1\n").arg(strictSettings ? QObject::tr("yes") : QObject::tr("no"));
  summary += QObject::tr("  (EQ + layout restored)\n");

  return summary;
}
} // namespace

SessionsDialog::SessionsDialog(PipeWireGraph* graph, QWidget* parent)
    : QDialog(parent)
    , m_graph(graph)
{
  setWindowTitle(tr("Sessions"));
  setModal(true);
  resize(720, 480);

  auto* root = new QVBoxLayout(this);

  auto* intro = new QLabel(
      tr("Sessions are named snapshots of your setup:\n"
         "patchbay links, default input/output, per-device EQ, and layout (device order + patchbay node positions)."),
      this);
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* box = new QGroupBox(tr("Snapshots"), this);
  auto* boxV = new QVBoxLayout(box);

  m_list = new QListWidget(box);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  boxV->addWidget(m_list, 1);

  auto* options = new QHBoxLayout();
  m_strictLinks = new QCheckBox(tr("Strict links"), box);
  m_strictLinks->setToolTip(tr("Disconnect other links touching ports in this snapshot"));
  options->addWidget(m_strictLinks);

  m_strictSettings = new QCheckBox(tr("Strict settings"), box);
  m_strictSettings->setToolTip(tr("Replace EQ/layout settings (clears existing before restoring)"));
  m_strictSettings->setChecked(true);
  options->addWidget(m_strictSettings);
  options->addStretch(1);
  boxV->addLayout(options);

  auto* actions = new QHBoxLayout();
  auto* saveBtn = new QPushButton(tr("Save Current…"), box);
  m_applyBtn = new QPushButton(tr("Apply"), box);
  m_deleteBtn = new QPushButton(tr("Delete"), box);
  actions->addWidget(saveBtn);
  actions->addWidget(m_applyBtn);
  actions->addWidget(m_deleteBtn);
  actions->addStretch(1);
  boxV->addLayout(actions);

  root->addWidget(box, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &SessionsDialog::reject);
  root->addWidget(buttons);

  connect(saveBtn, &QPushButton::clicked, this, &SessionsDialog::saveSnapshot);
  connect(m_applyBtn, &QPushButton::clicked, this, &SessionsDialog::applySnapshot);
  connect(m_deleteBtn, &QPushButton::clicked, this, &SessionsDialog::deleteSnapshot);

  connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
    const bool has = !selectedSnapshotName().isEmpty();
    if (m_applyBtn) {
      m_applyBtn->setEnabled(has);
    }
    if (m_deleteBtn) {
      m_deleteBtn->setEnabled(has);
    }
  });
  connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { applySnapshot(); });

  reloadSnapshots();
}

QString SessionsDialog::selectedSnapshotName() const
{
  if (!m_list) {
    return {};
  }
  auto* item = m_list->currentItem();
  if (!item) {
    return {};
  }
  return item->text().trimmed();
}

void SessionsDialog::reloadSnapshots()
{
  const QString prev = selectedSnapshotName();

  if (!m_list) {
    return;
  }

  QSettings s;
  const QStringList names = SessionSnapshotStore::listSnapshotNames(s);

  m_list->clear();
  for (const auto& name : names) {
    m_list->addItem(name);
  }

  int idx = -1;
  for (int i = 0; i < m_list->count(); ++i) {
    if (m_list->item(i)->text().trimmed() == prev) {
      idx = i;
      break;
    }
  }
  if (idx < 0 && m_list->count() > 0) {
    idx = 0;
  }
  if (idx >= 0) {
    m_list->setCurrentRow(idx);
  }

  const bool has = !selectedSnapshotName().isEmpty();
  if (m_applyBtn) {
    m_applyBtn->setEnabled(has);
  }
  if (m_deleteBtn) {
    m_deleteBtn->setEnabled(has);
  }
}

void SessionsDialog::saveSnapshot()
{
  if (!m_graph) {
    QMessageBox::warning(this, tr("Sessions"), tr("PipeWire graph is unavailable."));
    return;
  }

  const QString initial = selectedSnapshotName();
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("Save Snapshot"), tr("Snapshot name:"), QLineEdit::Normal, initial, &ok).trimmed();
  if (!ok || name.isEmpty()) {
    return;
  }

  QSettings s;
  const bool exists = SessionSnapshotStore::load(s, name).has_value();
  if (exists) {
    const auto r = QMessageBox::question(this,
                                        tr("Overwrite Snapshot"),
                                        tr("A snapshot named “%1” already exists.\nOverwrite it?").arg(name),
                                        QMessageBox::Yes | QMessageBox::No);
    if (r != QMessageBox::Yes) {
      return;
    }
  }

  const SessionSnapshot snap = snapshotSession(name, *m_graph, s);
  SessionSnapshotStore::save(s, snap);
  reloadSnapshots();
  emit snapshotsChanged();
}

void SessionsDialog::applySnapshot()
{
  if (!m_graph) {
    QMessageBox::warning(this, tr("Sessions"), tr("PipeWire graph is unavailable."));
    return;
  }

  const QString name = selectedSnapshotName();
  if (name.isEmpty()) {
    return;
  }

  QSettings s;
  const auto snap = SessionSnapshotStore::load(s, name);
  if (!snap) {
    QMessageBox::warning(this, tr("Sessions"), tr("Snapshot “%1” could not be loaded.").arg(name));
    reloadSnapshots();
    return;
  }

  const bool strictLinks = m_strictLinks && m_strictLinks->isChecked();
  const bool strictSettings = m_strictSettings && m_strictSettings->isChecked();
  const SessionSnapshotApplyResult r = applySessionSnapshot(*m_graph, s, *snap, strictLinks, strictSettings);

  const QString summary = summaryForApply(name, r, strictLinks, strictSettings);

  QMessageBox box(r.errors.isEmpty() ? QMessageBox::Information : QMessageBox::Warning, tr("Sessions"), summary, QMessageBox::Ok, this);
  box.setDetailedText(QStringList(r.missing + r.errors).join('\n'));
  box.exec();

  emit sessionApplied();
}

void SessionsDialog::deleteSnapshot()
{
  const QString name = selectedSnapshotName();
  if (name.isEmpty()) {
    return;
  }

  const auto r = QMessageBox::question(this,
                                      tr("Delete Snapshot"),
                                      tr("Delete snapshot “%1”?").arg(name),
                                      QMessageBox::Yes | QMessageBox::No);
  if (r != QMessageBox::Yes) {
    return;
  }

  QSettings s;
  const bool removed = SessionSnapshotStore::remove(s, name);
  if (!removed) {
    QMessageBox::warning(this, tr("Sessions"), tr("Snapshot “%1” was not found.").arg(name));
  }

  reloadSnapshots();
  emit snapshotsChanged();
}
