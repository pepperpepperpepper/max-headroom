#pragma once

#include <QMainWindow>

#include <cstdint>
#include <optional>

class QTabWidget;
class MixerPage;
class VisualizerPage;
class PatchbayPage;
class PipeWireThread;
class PipeWireGraph;
class AudioTap;
class AudioRecorder;
class EqManager;
class PatchbayAutoConnectController;
class QAction;
class QLabel;
class QMenu;
class QSlider;
class QSystemTrayIcon;
class QTimer;
class QWidgetAction;
class QCloseEvent;
class LogStore;

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(LogStore* logs, QWidget* parent = nullptr);
  ~MainWindow() override;

  bool selectTabByKey(const QString& key);
  void setVisualizerTapTarget(const QString& targetObject, bool captureSink);
  PipeWireGraph* graph() const { return m_graph; }

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void openSettings();
  void openEngine();
  void openSessions();
  void openRecorder();
  void openLogs();

  void setupTray();
  uint32_t trayOutputNodeId() const;
  void scheduleTrayRefresh();
  void refreshTrayUi();
  void showTabFromTray(const QString& key);
  void toggleTrayMute();
  void applyTrayVolumePercent(int percent);
  void rebuildTrayProfilesMenu();
  void requestExitFromTray();

  PipeWireThread* m_pw = nullptr;
  PipeWireGraph* m_graph = nullptr;
  AudioTap* m_tap = nullptr;
  AudioRecorder* m_recorder = nullptr;
  EqManager* m_eq = nullptr;
  PatchbayAutoConnectController* m_autoConnect = nullptr;
  LogStore* m_logs = nullptr;
  QTabWidget* m_tabs = nullptr;
  MixerPage* m_mixerPage = nullptr;
  VisualizerPage* m_visualizerPage = nullptr;
  PatchbayPage* m_patchbayPage = nullptr;

  bool m_trayExitRequested = false;
  QSystemTrayIcon* m_tray = nullptr;
  QMenu* m_trayMenu = nullptr;
  QMenu* m_trayProfilesMenu = nullptr;
  QAction* m_trayMuteAction = nullptr;
  QWidgetAction* m_trayVolumeAction = nullptr;
  QSlider* m_trayVolumeSlider = nullptr;
  QLabel* m_trayVolumeLabel = nullptr;
  QTimer* m_trayRefreshTimer = nullptr;

  uint32_t m_traySoftMuteNodeId = 0;
  std::optional<float> m_traySoftMuteRestoreVolume;
};
