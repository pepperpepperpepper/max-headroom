#pragma once

#include <QDialog>
#include <QVector>

#include "backend/EqConfig.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class EqResponseWidget;

class EqDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit EqDialog(const QString& deviceLabel, const EqPreset& initial, QWidget* parent = nullptr);

  EqPreset preset() const;

private:
  void resetToDefaults();
  void loadFromPreset(const EqPreset& p);
  EqPreset collectPreset() const;
  void refreshResponse();

  void loadPresetFromLibrary();
  void savePresetToLibrary();
  void importPresetFromTextFile();

  QCheckBox* m_enabled = nullptr;
  QDoubleSpinBox* m_preamp = nullptr;
  EqResponseWidget* m_response = nullptr;

  QVector<QCheckBox*> m_bandEnabled;
  QVector<QComboBox*> m_bandType;
  QVector<QDoubleSpinBox*> m_bandFreq;
  QVector<QDoubleSpinBox*> m_bandGain;
  QVector<QDoubleSpinBox*> m_bandQ;

  EqPreset m_defaults;
};
