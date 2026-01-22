#pragma once

#include <QWidget>

#include "backend/EqConfig.h"

class EqResponseWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit EqResponseWidget(QWidget* parent = nullptr);

  void setPreset(const EqPreset& preset);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  EqPreset m_preset;
  double m_sampleRate = 48000.0;
};

