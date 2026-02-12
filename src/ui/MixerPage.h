#pragma once

#include <QHash>
#include <QWidget>
#include <QPointer>

#include <cstdint>
#include <optional>

class QLineEdit;
class QScrollArea;
class QTimer;
class QComboBox;
class QPushButton;
class QSlider;
class QLabel;
class QCheckBox;
class QShowEvent;
class QHideEvent;
class QEvent;
class PipeWireGraph;
class EqManager;
class PipeWireThread;
class LevelMeterWidget;

class MixerPage final : public QWidget
{
  Q_OBJECT

public:
  explicit MixerPage(PipeWireThread* pw, PipeWireGraph* graph, EqManager* eq, QWidget* parent = nullptr);

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

signals:
  void visualizerTapRequested(QString targetObject, bool captureSink);

public slots:
  void refresh();
  void updateForWindowVisibilityChange();

  private:
    struct RebuildSnapshot final {
      QString filter;
      bool defaultDeviceSupported = false;
      uint32_t defaultSinkId = 0;
      uint32_t defaultSourceId = 0;
      QVector<uint32_t> playback;
      QVector<uint32_t> recording;
      QVector<uint32_t> outputs;
      QVector<uint32_t> inputs;
      QVector<uint32_t> other;

      bool operator==(const RebuildSnapshot& rhs) const
      {
        return filter == rhs.filter && defaultDeviceSupported == rhs.defaultDeviceSupported && defaultSinkId == rhs.defaultSinkId &&
            defaultSourceId == rhs.defaultSourceId && playback == rhs.playback && recording == rhs.recording && outputs == rhs.outputs &&
            inputs == rhs.inputs && other == rhs.other;
      }
      bool operator!=(const RebuildSnapshot& rhs) const { return !(*this == rhs); }
    };

    enum class MeterMode {
      Off = 0,
      SelectedOnly = 1,
      VisibleRows = 2,
      All = 3,
    };

    void disableAllMeters();
    void loadMeterMode();
    enum class MeterStyle {
      SimplePeak = 0,
      DetailedPeakRms = 1,
    };
    void loadMeterStyle();
    void applyMeterStyle();
    void updateMeterActives();
    void scheduleRebuild();
    void refreshControls();
    void rebuild();
  void tickMeters();

  PipeWireThread* m_pw = nullptr;
  PipeWireGraph* m_graph = nullptr;
  EqManager* m_eq = nullptr;
  QLineEdit* m_filter = nullptr;
  QComboBox* m_defaultOutput = nullptr;
  QPushButton* m_setDefaultOutput = nullptr;
  QComboBox* m_defaultInput = nullptr;
  QPushButton* m_setDefaultInput = nullptr;
  QComboBox* m_meterMode = nullptr;
  QComboBox* m_meterStyle = nullptr;
  QScrollArea* m_scroll = nullptr;
  QWidget* m_container = nullptr;
  QTimer* m_rebuildTimer = nullptr;
  QTimer* m_meterTimer = nullptr;
  QList<QPointer<LevelMeterWidget>> m_meters;
  MeterMode m_meterModeValue = MeterMode::VisibleRows;
  MeterStyle m_meterStyleValue = MeterStyle::SimplePeak;
  QPointer<QWidget> m_selectedRow;
  bool m_pendingRebuild = false;
  bool m_windowFilterInstalled = false;

  QHash<uint32_t, QPointer<QSlider>> m_volumeSliders;
  QHash<uint32_t, QPointer<QLabel>> m_volumePcts;
  QHash<uint32_t, QPointer<QCheckBox>> m_mutes;

  std::optional<RebuildSnapshot> m_lastSnapshot;
};
