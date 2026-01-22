#include "LogsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

#include "backend/LogStore.h"

LogsDialog::LogsDialog(LogStore* logs, QWidget* parent)
    : QDialog(parent)
    , m_logs(logs)
{
  setWindowTitle(tr("Log / Console"));
  setModal(false);
  resize(860, 520);

  rebuildUi();
  reload();

  if (m_logs) {
    connect(m_logs, &LogStore::lineAdded, this, &LogsDialog::appendLine);
    connect(m_logs, &LogStore::cleared, this, [this]() {
      if (m_view) {
        m_view->clear();
      }
    });
  }
}

LogsDialog::~LogsDialog() = default;

void LogsDialog::rebuildUi()
{
  auto* root = new QVBoxLayout(this);

  auto* actions = new QWidget(this);
  auto* actionsLayout = new QHBoxLayout(actions);
  actionsLayout->setContentsMargins(0, 0, 0, 0);

  m_followTail = new QCheckBox(tr("Follow tail"), actions);
  m_followTail->setChecked(true);
  actionsLayout->addWidget(m_followTail);
  actionsLayout->addStretch(1);

  m_clearButton = new QPushButton(tr("Clear"), actions);
  connect(m_clearButton, &QPushButton::clicked, this, &LogsDialog::clearLogs);
  actionsLayout->addWidget(m_clearButton);

  m_copyButton = new QPushButton(tr("Copy"), actions);
  connect(m_copyButton, &QPushButton::clicked, this, &LogsDialog::copyAll);
  actionsLayout->addWidget(m_copyButton);

  m_saveButton = new QPushButton(tr("Save…"), actions);
  connect(m_saveButton, &QPushButton::clicked, this, &LogsDialog::saveToFile);
  actionsLayout->addWidget(m_saveButton);

  root->addWidget(actions);

  m_view = new QPlainTextEdit(this);
  m_view->setReadOnly(true);
  m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_view->setMaximumBlockCount(2500);
  m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  root->addWidget(m_view, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void LogsDialog::reload()
{
  if (!m_logs || !m_view) {
    return;
  }
  const QStringList lines = m_logs->lines();
  m_view->setPlainText(lines.join('\n'));
  if (m_followTail && m_followTail->isChecked()) {
    QTextCursor c = m_view->textCursor();
    c.movePosition(QTextCursor::End);
    m_view->setTextCursor(c);
  }
}

void LogsDialog::appendLine(const QString& line)
{
  if (!m_view) {
    return;
  }
  m_view->appendPlainText(line);
  if (m_followTail && m_followTail->isChecked()) {
    QTextCursor c = m_view->textCursor();
    c.movePosition(QTextCursor::End);
    m_view->setTextCursor(c);
  }
}

void LogsDialog::clearLogs()
{
  if (m_logs) {
    m_logs->clear();
  } else if (m_view) {
    m_view->clear();
  }
}

void LogsDialog::copyAll()
{
  if (!m_view) {
    return;
  }
  if (auto* cb = QApplication::clipboard()) {
    cb->setText(m_view->toPlainText());
  }
}

void LogsDialog::saveToFile()
{
  if (!m_view) {
    return;
  }

  const QString path = QFileDialog::getSaveFileName(this, tr("Save log"), QStringLiteral("headroom.log"));
  if (path.trimmed().isEmpty()) {
    return;
  }

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    return;
  }

  const QByteArray data = m_view->toPlainText().toUtf8();
  f.write(data);
  f.write("\n");
}

