#pragma once

#include <QSlider>

class VolumeSlider final : public QSlider {
public:
  explicit VolumeSlider(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
  void paintEvent(QPaintEvent* event) override;
};

