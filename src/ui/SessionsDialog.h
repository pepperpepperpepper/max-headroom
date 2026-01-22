#pragma once

#include <QDialog>

class PipeWireGraph;
class QListWidget;
class QPushButton;
class QCheckBox;

class SessionsDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit SessionsDialog(PipeWireGraph* graph, QWidget* parent = nullptr);

signals:
  void sessionApplied();
  void snapshotsChanged();

private:
  void reloadSnapshots();
  QString selectedSnapshotName() const;

  void saveSnapshot();
  void applySnapshot();
  void deleteSnapshot();

  PipeWireGraph* m_graph = nullptr;

  QListWidget* m_list = nullptr;
  QPushButton* m_applyBtn = nullptr;
  QPushButton* m_deleteBtn = nullptr;
  QCheckBox* m_strictLinks = nullptr;
  QCheckBox* m_strictSettings = nullptr;
};

