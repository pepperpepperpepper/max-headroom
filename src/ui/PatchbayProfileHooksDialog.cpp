#include "PatchbayProfileHooksDialog.h"

#include "backend/PatchbayProfileHooks.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

PatchbayProfileHooksDialog::PatchbayProfileHooksDialog(const QString& profileName, QWidget* parent)
    : QDialog(parent)
    , m_profileName(profileName.trimmed())
{
  setWindowTitle(tr("Profile Hooks"));
  setModal(true);
  resize(720, 280);

  auto* root = new QVBoxLayout(this);

  auto* help = new QLabel(
      tr("Commands run when this Patchbay profile is unloaded/loaded.\n"
         "They are executed via: sh -lc <command>.\n"
         "Environment variables: HEADROOM_HOOK (load|unload), HEADROOM_PROFILE, HEADROOM_PROFILE_PREV, HEADROOM_PROFILE_NEXT."),
      this);
  help->setWordWrap(true);
  root->addWidget(help);

  auto* box = new QGroupBox(tr("Hooks for “%1”").arg(m_profileName), this);
  auto* form = new QFormLayout(box);

  auto mkRow = [&](QLineEdit** outEdit, const QString& placeholder) -> QWidget* {
    auto* row = new QWidget(box);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* edit = new QLineEdit(row);
    edit->setPlaceholderText(placeholder);
    h->addWidget(edit, 1);
    auto* browse = new QPushButton(tr("Browse…"), row);
    h->addWidget(browse, 0);
    connect(browse, &QPushButton::clicked, this, [this, edit]() { browseScriptPath(edit, this); });
    *outEdit = edit;
    return row;
  };

  form->addRow(tr("On unload:"), mkRow(&m_onUnload, tr("e.g. pkill -f my-synth || true")));
  form->addRow(tr("On load:"), mkRow(&m_onLoad, tr("e.g. ~/.config/headroom/hooks/start-profile.sh")));

  root->addWidget(box);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    save();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);

  load();
}

void PatchbayProfileHooksDialog::browseScriptPath(QLineEdit* edit, QWidget* parent)
{
  if (!edit) {
    return;
  }

  const QString initial = edit->text().trimmed();
  const QString startDir = initial.isEmpty() ? QString{} : QFileInfo(initial).absolutePath();

  const QString path =
      QFileDialog::getOpenFileName(parent, QObject::tr("Select Script"), startDir, QObject::tr("All Files (*)"));
  if (path.isEmpty()) {
    return;
  }
  edit->setText(path);
}

void PatchbayProfileHooksDialog::load()
{
  if (m_profileName.isEmpty()) {
    return;
  }
  QSettings s;
  const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, m_profileName);
  if (m_onLoad) {
    m_onLoad->setText(h.onLoadCommand);
  }
  if (m_onUnload) {
    m_onUnload->setText(h.onUnloadCommand);
  }
}

void PatchbayProfileHooksDialog::save()
{
  if (m_profileName.isEmpty()) {
    return;
  }
  PatchbayProfileHooks h;
  if (m_onLoad) {
    h.onLoadCommand = m_onLoad->text();
  }
  if (m_onUnload) {
    h.onUnloadCommand = m_onUnload->text();
  }

  QSettings s;
  PatchbayProfileHooksStore::save(s, m_profileName, h);
}
