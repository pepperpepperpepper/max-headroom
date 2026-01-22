#pragma once

#include <QDialog>

class QLineEdit;

class PatchbayProfileHooksDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit PatchbayProfileHooksDialog(const QString& profileName, QWidget* parent = nullptr);

private:
  void load();
  void save();
  static void browseScriptPath(QLineEdit* edit, QWidget* parent);

  QString m_profileName;
  QLineEdit* m_onLoad = nullptr;
  QLineEdit* m_onUnload = nullptr;
};

