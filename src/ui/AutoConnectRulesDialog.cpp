#include "AutoConnectRulesDialog.h"

#include "backend/PipeWireGraph.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QStringList splitLines(const QString& text)
{
  QStringList out;
  const QStringList lines = text.split('\n');
  out.reserve(lines.size());
  for (const auto& line : lines) {
    const QString t = line.trimmed();
    if (!t.isEmpty()) {
      out.push_back(t);
    }
  }
  out.removeDuplicates();
  std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
  return out;
}

QString joinLines(const QStringList& lines)
{
  QStringList out;
  out.reserve(lines.size());
  for (const auto& l : lines) {
    const QString t = l.trimmed();
    if (!t.isEmpty()) {
      out.push_back(t);
    }
  }
  return out.join('\n');
}

QString ruleSummaryOut(const AutoConnectRule& r)
{
  return QStringLiteral("%1 / %2").arg(r.outputNodeRegex, r.outputPortRegex);
}

QString ruleSummaryIn(const AutoConnectRule& r)
{
  return QStringLiteral("%1 / %2").arg(r.inputNodeRegex, r.inputPortRegex);
}

class AutoConnectRuleEditDialog final : public QDialog
{
public:
  AutoConnectRuleEditDialog(const AutoConnectRule& initial, const QStringList& existingNames, QWidget* parent = nullptr)
      : QDialog(parent)
      , m_initialName(initial.name.trimmed())
      , m_existingNames(existingNames)
  {
    setWindowTitle(tr("Auto-Connect Rule"));
    setModal(true);
    resize(560, 240);

    auto* root = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    root->addLayout(form, 1);

    m_name = new QLineEdit(this);
    m_name->setText(initial.name);
    form->addRow(tr("Name:"), m_name);

    m_enabled = new QCheckBox(tr("Enabled"), this);
    m_enabled->setChecked(initial.enabled);
    form->addRow(QString{}, m_enabled);

    m_outNode = new QLineEdit(this);
    m_outNode->setText(initial.outputNodeRegex);
    form->addRow(tr("Output node regex:"), m_outNode);

    m_outPort = new QLineEdit(this);
    m_outPort->setText(initial.outputPortRegex);
    form->addRow(tr("Output port regex:"), m_outPort);

    m_inNode = new QLineEdit(this);
    m_inNode->setText(initial.inputNodeRegex);
    form->addRow(tr("Input node regex:"), m_inNode);

    m_inPort = new QLineEdit(this);
    m_inPort->setText(initial.inputPortRegex);
    form->addRow(tr("Input port regex:"), m_inPort);

    auto* help = new QLabel(
        tr("Node regex matches: node.name, description, appName, mediaClass.\nPort regex matches: port.name, audio channel."),
        this);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #64748b;"));
    root->addWidget(help);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &AutoConnectRuleEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &AutoConnectRuleEditDialog::reject);
    root->addWidget(buttons);
  }

  AutoConnectRule rule() const
  {
    AutoConnectRule r;
    r.name = m_name ? m_name->text().trimmed() : QString{};
    r.enabled = m_enabled ? m_enabled->isChecked() : true;
    r.outputNodeRegex = m_outNode ? m_outNode->text().trimmed() : QString{};
    r.outputPortRegex = m_outPort ? m_outPort->text().trimmed() : QString{};
    r.inputNodeRegex = m_inNode ? m_inNode->text().trimmed() : QString{};
    r.inputPortRegex = m_inPort ? m_inPort->text().trimmed() : QString{};
    return r;
  }

protected:
  void accept() override
  {
    const AutoConnectRule r = rule();

    if (r.name.isEmpty()) {
      QMessageBox::warning(this, tr("Auto-Connect Rule"), tr("Rule name is required."));
      return;
    }

    const bool isRename = (r.name != m_initialName);
    if (isRename && m_existingNames.contains(r.name)) {
      QMessageBox::warning(this, tr("Auto-Connect Rule"), tr("A rule named “%1” already exists.").arg(r.name));
      return;
    }

    auto checkRe = [this](const QString& value, const QString& label) -> bool {
      if (value.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Auto-Connect Rule"), tr("%1 is required.").arg(label));
        return false;
      }
      QRegularExpression re(value);
      if (!re.isValid()) {
        QMessageBox::warning(this, tr("Auto-Connect Rule"), tr("%1 is not a valid regex.").arg(label));
        return false;
      }
      return true;
    };

    if (!checkRe(r.outputNodeRegex, tr("Output node regex")) || !checkRe(r.outputPortRegex, tr("Output port regex")) ||
        !checkRe(r.inputNodeRegex, tr("Input node regex")) || !checkRe(r.inputPortRegex, tr("Input port regex"))) {
      return;
    }

    QDialog::accept();
  }

private:
  QString m_initialName;
  QStringList m_existingNames;
  QLineEdit* m_name = nullptr;
  QCheckBox* m_enabled = nullptr;
  QLineEdit* m_outNode = nullptr;
  QLineEdit* m_outPort = nullptr;
  QLineEdit* m_inNode = nullptr;
  QLineEdit* m_inPort = nullptr;
};
} // namespace

AutoConnectRulesDialog::AutoConnectRulesDialog(PipeWireGraph* graph, QWidget* parent)
    : QDialog(parent)
    , m_graph(graph)
{
  setWindowTitle(tr("Patchbay Auto-Connect"));
  setModal(true);
  resize(820, 560);

  auto* root = new QVBoxLayout(this);

  auto* intro = new QLabel(
      tr("Automatically connect ports when nodes appear.\n"
         "Rules match nodes/ports by regex, and can be filtered by endpoint whitelist/blacklist."),
      this);
  intro->setWordWrap(true);
  root->addWidget(intro);

  auto* topRow = new QHBoxLayout();
  m_enabled = new QCheckBox(tr("Enable auto-connect rules"), this);
  topRow->addWidget(m_enabled);
  topRow->addStretch(1);
  m_applyBtn = new QPushButton(tr("Apply Now"), this);
  topRow->addWidget(m_applyBtn);
  root->addLayout(topRow);

  connect(m_applyBtn, &QPushButton::clicked, this, &AutoConnectRulesDialog::applyNow);

  auto* filters = new QGroupBox(tr("Endpoint Filters (optional)"), this);
  auto* filtersForm = new QFormLayout(filters);

  m_whitelist = new QPlainTextEdit(filters);
  m_whitelist->setPlaceholderText(tr("One regex per line (matches “node.name:port.name”)"));
  m_whitelist->setMinimumHeight(70);
  filtersForm->addRow(tr("Whitelist:"), m_whitelist);

  m_blacklist = new QPlainTextEdit(filters);
  m_blacklist->setPlaceholderText(tr("One regex per line (matches “node.name:port.name”)"));
  m_blacklist->setMinimumHeight(70);
  filtersForm->addRow(tr("Blacklist:"), m_blacklist);

  root->addWidget(filters);

  auto* rulesBox = new QGroupBox(tr("Rules"), this);
  auto* rulesV = new QVBoxLayout(rulesBox);

  m_rules = new QTreeWidget(rulesBox);
  m_rules->setColumnCount(3);
  m_rules->setHeaderLabels({tr("Rule"), tr("Output (node/port regex)"), tr("Input (node/port regex)")});
  m_rules->setRootIsDecorated(false);
  m_rules->setUniformRowHeights(true);
  m_rules->setAllColumnsShowFocus(true);
  if (auto* h = m_rules->header()) {
    h->setStretchLastSection(true);
    h->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    h->setSectionResizeMode(1, QHeaderView::Stretch);
    h->setSectionResizeMode(2, QHeaderView::Stretch);
  }
  rulesV->addWidget(m_rules, 1);

  auto* actions = new QHBoxLayout();
  auto* addBtn = new QPushButton(tr("Add…"), rulesBox);
  m_editBtn = new QPushButton(tr("Edit…"), rulesBox);
  m_deleteBtn = new QPushButton(tr("Delete"), rulesBox);
  actions->addWidget(addBtn);
  actions->addWidget(m_editBtn);
  actions->addWidget(m_deleteBtn);
  actions->addStretch(1);
  rulesV->addLayout(actions);

  root->addWidget(rulesBox, 1);

  connect(addBtn, &QPushButton::clicked, this, &AutoConnectRulesDialog::addRule);
  connect(m_editBtn, &QPushButton::clicked, this, &AutoConnectRulesDialog::editSelectedRule);
  connect(m_deleteBtn, &QPushButton::clicked, this, &AutoConnectRulesDialog::deleteSelectedRule);

  connect(m_rules, &QTreeWidget::itemSelectionChanged, this, [this]() {
    const bool has = selectedRuleIndex().has_value();
    if (m_editBtn) {
      m_editBtn->setEnabled(has);
    }
    if (m_deleteBtn) {
      m_deleteBtn->setEnabled(has);
    }
  });
  connect(m_rules, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { editSelectedRule(); });
  connect(m_rules, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
    if (!item || column != 0) {
      return;
    }
    const QString name = item->data(0, Qt::UserRole).toString();
    if (name.isEmpty()) {
      return;
    }
    for (auto& r : m_cfg.rules) {
      if (r.name == name) {
        r.enabled = (item->checkState(0) == Qt::Checked);
        break;
      }
    }
  });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &AutoConnectRulesDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &AutoConnectRulesDialog::reject);
  root->addWidget(buttons);

  loadFromSettings();
}

void AutoConnectRulesDialog::loadFromSettings()
{
  QSettings s;
  m_cfg = loadAutoConnectConfig(s);
  applyConfigToUi(m_cfg);
}

void AutoConnectRulesDialog::saveToSettings()
{
  QSettings s;
  m_cfg = configFromUi();
  saveAutoConnectConfig(s, m_cfg);
}

void AutoConnectRulesDialog::reloadRulesList()
{
  if (!m_rules) {
    return;
  }

  m_rules->blockSignals(true);
  m_rules->clear();

  QVector<AutoConnectRule> sorted = m_cfg.rules;
  std::sort(sorted.begin(), sorted.end(), [](const AutoConnectRule& a, const AutoConnectRule& b) { return a.name.toLower() < b.name.toLower(); });

  for (const auto& r : sorted) {
    auto* item = new QTreeWidgetItem(m_rules);
    item->setText(0, r.name);
    item->setText(1, ruleSummaryOut(r));
    item->setText(2, ruleSummaryIn(r));
    item->setData(0, Qt::UserRole, r.name);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    item->setCheckState(0, r.enabled ? Qt::Checked : Qt::Unchecked);
    m_rules->addTopLevelItem(item);
  }

  m_rules->blockSignals(false);

  if (m_editBtn) {
    m_editBtn->setEnabled(false);
  }
  if (m_deleteBtn) {
    m_deleteBtn->setEnabled(false);
  }
}

std::optional<int> AutoConnectRulesDialog::selectedRuleIndex() const
{
  if (!m_rules) {
    return std::nullopt;
  }
  const auto items = m_rules->selectedItems();
  if (items.isEmpty() || !items.first()) {
    return std::nullopt;
  }
  const QString name = items.first()->data(0, Qt::UserRole).toString();
  if (name.isEmpty()) {
    return std::nullopt;
  }
  for (int i = 0; i < m_cfg.rules.size(); ++i) {
    if (m_cfg.rules[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

AutoConnectConfig AutoConnectRulesDialog::configFromUi() const
{
  AutoConnectConfig cfg = m_cfg;
  cfg.enabled = m_enabled && m_enabled->isChecked();
  cfg.whitelist = m_whitelist ? splitLines(m_whitelist->toPlainText()) : QStringList{};
  cfg.blacklist = m_blacklist ? splitLines(m_blacklist->toPlainText()) : QStringList{};
  return cfg;
}

void AutoConnectRulesDialog::applyConfigToUi(const AutoConnectConfig& cfg)
{
  if (m_enabled) {
    m_enabled->setChecked(cfg.enabled);
  }
  if (m_whitelist) {
    m_whitelist->setPlainText(joinLines(cfg.whitelist));
  }
  if (m_blacklist) {
    m_blacklist->setPlainText(joinLines(cfg.blacklist));
  }
  reloadRulesList();
}

void AutoConnectRulesDialog::addRule()
{
  QStringList names;
  names.reserve(m_cfg.rules.size());
  for (const auto& r : m_cfg.rules) {
    names.push_back(r.name);
  }

  AutoConnectRule initial;
  initial.enabled = true;
  initial.outputNodeRegex = QStringLiteral(".*");
  initial.outputPortRegex = QStringLiteral(".*");
  initial.inputNodeRegex = QStringLiteral(".*");
  initial.inputPortRegex = QStringLiteral(".*");

  AutoConnectRuleEditDialog dlg(initial, names, this);
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const AutoConnectRule r = dlg.rule();
  m_cfg.rules.push_back(r);
  reloadRulesList();
}

void AutoConnectRulesDialog::editSelectedRule()
{
  const auto idxOpt = selectedRuleIndex();
  if (!idxOpt) {
    return;
  }
  const int idx = *idxOpt;

  QStringList names;
  names.reserve(m_cfg.rules.size());
  for (const auto& r : m_cfg.rules) {
    if (r.name != m_cfg.rules[idx].name) {
      names.push_back(r.name);
    }
  }

  AutoConnectRuleEditDialog dlg(m_cfg.rules[idx], names, this);
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const AutoConnectRule edited = dlg.rule();
  m_cfg.rules[idx] = edited;
  reloadRulesList();
}

void AutoConnectRulesDialog::deleteSelectedRule()
{
  const auto idxOpt = selectedRuleIndex();
  if (!idxOpt) {
    return;
  }
  const int idx = *idxOpt;

  const QString name = m_cfg.rules[idx].name;
  const auto choice = QMessageBox::question(
      this,
      tr("Delete Rule"),
      tr("Delete auto-connect rule “%1”?").arg(name),
      QMessageBox::Yes | QMessageBox::No,
      QMessageBox::No);
  if (choice != QMessageBox::Yes) {
    return;
  }

  m_cfg.rules.removeAt(idx);
  reloadRulesList();
}

void AutoConnectRulesDialog::applyNow()
{
  if (!m_graph) {
    return;
  }

  const AutoConnectConfig cfg = configFromUi();
  const AutoConnectApplyResult r = applyAutoConnectRules(*m_graph, cfg);

  QString summary = tr("Auto-connect applied.\nCreated: %1\nAlready present: %2\nErrors: %3")
                        .arg(r.linksCreated)
                        .arg(r.linksAlreadyPresent)
                        .arg(r.errors.size());

  QMessageBox box(QMessageBox::Information, tr("Auto-Connect"), summary, QMessageBox::Ok, this);
  if (!r.errors.isEmpty()) {
    QString details = tr("Errors:\n");
    for (const auto& e : r.errors) {
      details += QStringLiteral("  - %1\n").arg(e);
    }
    box.setDetailedText(details);
  }
  box.exec();
}

void AutoConnectRulesDialog::accept()
{
  saveToSettings();
  QDialog::accept();
}

