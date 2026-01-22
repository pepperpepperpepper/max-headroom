#pragma once

#include <QDialog>

#include "backend/PatchbayAutoConnectRules.h"

class PipeWireGraph;
class QCheckBox;
class QPlainTextEdit;
class QTreeWidget;
class QPushButton;

class AutoConnectRulesDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit AutoConnectRulesDialog(PipeWireGraph* graph, QWidget* parent = nullptr);

private:
  void loadFromSettings();
  void saveToSettings();
  void accept() override;

  void reloadRulesList();
  std::optional<int> selectedRuleIndex() const;

  void addRule();
  void editSelectedRule();
  void deleteSelectedRule();
  void applyNow();

  AutoConnectConfig configFromUi() const;
  void applyConfigToUi(const AutoConnectConfig& cfg);

  PipeWireGraph* m_graph = nullptr;
  AutoConnectConfig m_cfg;

  QCheckBox* m_enabled = nullptr;
  QPlainTextEdit* m_whitelist = nullptr;
  QPlainTextEdit* m_blacklist = nullptr;
  QTreeWidget* m_rules = nullptr;
  QPushButton* m_editBtn = nullptr;
  QPushButton* m_deleteBtn = nullptr;
  QPushButton* m_applyBtn = nullptr;
};
