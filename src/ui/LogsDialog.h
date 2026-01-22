#pragma once

#include <QDialog>

class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class LogStore;

class LogsDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit LogsDialog(LogStore* logs, QWidget* parent = nullptr);
  ~LogsDialog() override;

  LogsDialog(const LogsDialog&) = delete;
  LogsDialog& operator=(const LogsDialog&) = delete;

private:
  void rebuildUi();
  void reload();

  void appendLine(const QString& line);
  void clearLogs();
  void copyAll();
  void saveToFile();

  LogStore* m_logs = nullptr;
  QPlainTextEdit* m_view = nullptr;
  QCheckBox* m_followTail = nullptr;
  QPushButton* m_clearButton = nullptr;
  QPushButton* m_copyButton = nullptr;
  QPushButton* m_saveButton = nullptr;
};

