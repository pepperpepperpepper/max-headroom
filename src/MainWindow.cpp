#include "MainWindow.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QToolBar>
#include <QTimer>
#include <QWidgetAction>

#include "backend/AudioRecorder.h"
#include "backend/AudioTap.h"
#include "backend/EqManager.h"
#include "backend/LogStore.h"
#include "backend/PatchbayAutoConnectController.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PatchbayProfiles.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "settings/SettingsKeys.h"
#include "settings/VisualizerSettings.h"
#include "ui/AppTheme.h"
#include "ui/SettingsDialog.h"
#include "ui/EngineDialog.h"
#include "ui/SessionsDialog.h"
#include "ui/RecorderDialog.h"
#include "ui/LogsDialog.h"
#include "ui/GraphPage.h"
#include "ui/MixerPage.h"
#include "ui/PatchbayPage.h"
#include "ui/VisualizerPage.h"

MainWindow::MainWindow(LogStore* logs, QWidget* parent)
    : QMainWindow(parent)
{
  m_logs = logs;
  m_pw = new PipeWireThread(this);
  m_graph = new PipeWireGraph(m_pw, this);
  m_tap = new AudioTap(m_pw, this);
  m_recorder = new AudioRecorder(m_pw, this);
  m_eq = new EqManager(m_pw, m_graph, this);
  m_autoConnect = new PatchbayAutoConnectController(m_graph, this);

  m_tabs = new QTabWidget(this);
  m_mixerPage = new MixerPage(m_pw, m_graph, m_eq, this);
  m_tabs->addTab(m_mixerPage, tr("Mixer"));
  m_visualizerPage = new VisualizerPage(m_graph, m_tap, this);
  m_tabs->addTab(m_visualizerPage, tr("Visualizer"));
  m_patchbayPage = new PatchbayPage(m_graph, this);
  m_tabs->addTab(m_patchbayPage, tr("Patchbay"));
  m_tabs->addTab(new GraphPage(m_graph, this), tr("Graph"));
  setCentralWidget(m_tabs);

  if (m_logs) {
    connect(m_pw, &PipeWireThread::errorOccurred, this, [this](const QString& msg) {
      if (m_logs) {
        m_logs->append(LogStore::Level::Error, QStringLiteral("Headroom/PipeWire"), msg);
      }
    });
    connect(m_pw, &PipeWireThread::connectionChanged, this, [this](bool connected) {
      if (m_logs) {
        m_logs->append(LogStore::Level::Info,
                       QStringLiteral("Headroom/PipeWire"),
                       connected ? QStringLiteral("Connected") : QStringLiteral("Disconnected"));
      }
    });
    connect(m_recorder, &AudioRecorder::errorOccurred, this, [this](const QString& msg) {
      if (m_logs) {
        m_logs->append(LogStore::Level::Error, QStringLiteral("Headroom/Recorder"), msg);
      }
    });
  }

  connect(m_mixerPage, &MixerPage::visualizerTapRequested, this, [this](const QString& targetObject, bool captureSink) {
    if (m_visualizerPage) {
      m_visualizerPage->setTapTarget(targetObject, captureSink);
    } else if (m_tap) {
      m_tap->setTarget(captureSink, targetObject);
    }
    if (m_tabs) {
      m_tabs->setCurrentIndex(1);
    }
  });

  auto* toolbar = addToolBar(tr("Main"));
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  QAction* settingsAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("preferences-system")), tr("Settings…"));
  settingsAction->setShortcut(QKeySequence::Preferences);
  connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

  QAction* sessionsAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("document-open-recent")), tr("Sessions…"));
  connect(sessionsAction, &QAction::triggered, this, &MainWindow::openSessions);

  QAction* engineAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("system-run")), tr("Engine…"));
  connect(engineAction, &QAction::triggered, this, &MainWindow::openEngine);

  QAction* recordAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("media-record")), tr("Record…"));
  connect(recordAction, &QAction::triggered, this, &MainWindow::openRecorder);

  QAction* logsAction = toolbar->addAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")), tr("Logs…"));
  connect(logsAction, &QAction::triggered, this, &MainWindow::openLogs);

  setWindowTitle(tr("Headroom"));

  setupTray();

  connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
    QSettings s;
    const QString active = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();
    if (active.isEmpty()) {
      return;
    }
    const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, active);
    const PatchbayProfileHookStartResult r =
        startPatchbayProfileHookDetached(active, QString{}, QString{}, PatchbayProfileHookEvent::Unload, h.onUnloadCommand);
    if (m_logs) {
      if (r.started) {
        m_logs->append(LogStore::Level::Info, QStringLiteral("Headroom/Hooks"), tr("Unload hook started for “%1” (pid %2)").arg(active).arg(r.pid));
      } else if (!r.error.isEmpty()) {
        m_logs->append(LogStore::Level::Error, QStringLiteral("Headroom/Hooks"), tr("Unload hook failed for “%1”: %2").arg(active, r.error));
      }
    }
  });
}

void MainWindow::setVisualizerTapTarget(const QString& targetObject, bool captureSink)
{
  if (m_visualizerPage) {
    m_visualizerPage->setTapTarget(targetObject, captureSink);
  } else if (m_tap) {
    m_tap->setTarget(captureSink, targetObject);
  }
}

MainWindow::~MainWindow() = default;

bool MainWindow::selectTabByKey(const QString& key)
{
  if (!m_tabs) {
    return false;
  }

  const QString k = key.trimmed().toLower();
  int index = -1;
  if (k == QStringLiteral("mixer")) {
    index = 0;
  } else if (k == QStringLiteral("visualizer")) {
    index = 1;
  } else if (k == QStringLiteral("patchbay")) {
    index = 2;
  } else if (k == QStringLiteral("graph")) {
    index = 3;
  } else {
    return false;
  }

  m_tabs->setCurrentIndex(index);
  return true;
}

void MainWindow::openSettings()
{
  SettingsDialog dlg(m_graph, this);
  connect(&dlg, &SettingsDialog::uiThemeChanged, this, []() { AppTheme::applyFromSettings(); });
  connect(&dlg, &SettingsDialog::sinksOrderChanged, this, [this]() {
    if (m_mixerPage) {
      m_mixerPage->refresh();
    }
    if (m_patchbayPage) {
      m_patchbayPage->refresh();
    }
  });
  connect(&dlg, &SettingsDialog::layoutSettingsChanged, this, [this]() {
    if (m_patchbayPage) {
      m_patchbayPage->refresh();
    }
  });
  connect(&dlg, &SettingsDialog::visualizerSettingsChanged, this, [this]() {
    QSettings s;
    const VisualizerSettings cfg = VisualizerSettingsStore::load(s);
    if (m_visualizerPage) {
      m_visualizerPage->applySettings(cfg);
    } else if (m_tap) {
      m_tap->applySettings(cfg);
    }
  });
  dlg.exec();
}

void MainWindow::openSessions()
{
  SessionsDialog dlg(m_graph, this);
  connect(&dlg, &SessionsDialog::sessionApplied, this, [this]() {
    if (m_eq) {
      m_eq->refresh();
    }
    if (m_mixerPage) {
      m_mixerPage->refresh();
    }
    if (m_patchbayPage) {
      m_patchbayPage->refresh();
    }
  });
  dlg.exec();
}

void MainWindow::openEngine()
{
  EngineDialog dlg(m_graph, this);
  dlg.exec();
}

void MainWindow::openRecorder()
{
  RecorderDialog dlg(m_graph, m_recorder, this);
  dlg.exec();
}

void MainWindow::openLogs()
{
  if (!m_logs) {
    return;
  }
  auto* dlg = new LogsDialog(m_logs, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->show();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  if (m_tray && !m_trayExitRequested) {
    event->ignore();
    hide();

    QSettings s;
    const bool alreadyNotified = s.value(QStringLiteral("tray/closeNotified"), false).toBool();
    if (!alreadyNotified) {
      s.setValue(QStringLiteral("tray/closeNotified"), true);
      m_tray->showMessage(tr("Headroom"), tr("Headroom is still running in the tray.\nUse the tray icon menu to quit."), QSystemTrayIcon::Information);
    }
    return;
  }

  QMainWindow::closeEvent(event);
}

void MainWindow::setupTray()
{
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    return;
  }

  m_tray = new QSystemTrayIcon(this);
  m_tray->setToolTip(tr("Headroom"));
  QIcon trayIcon = QIcon::fromTheme(QStringLiteral("audio-volume-high"));
  if (trayIcon.isNull()) {
    trayIcon = QIcon(QStringLiteral(":/icons/app.svg"));
  }
  m_tray->setIcon(trayIcon);

  m_trayMenu = new QMenu(this);
  connect(m_trayMenu, &QMenu::aboutToShow, this, &MainWindow::refreshTrayUi);

  auto* openMixer = m_trayMenu->addAction(tr("Open Mixer"));
  connect(openMixer, &QAction::triggered, this, [this]() { showTabFromTray(QStringLiteral("mixer")); });

  auto* openPatchbay = m_trayMenu->addAction(tr("Open Patchbay"));
  connect(openPatchbay, &QAction::triggered, this, [this]() { showTabFromTray(QStringLiteral("patchbay")); });

  m_trayMenu->addSeparator();

  m_trayMuteAction = m_trayMenu->addAction(tr("Mute Output"));
  m_trayMuteAction->setCheckable(true);
  connect(m_trayMuteAction, &QAction::triggered, this, &MainWindow::toggleTrayMute);

  m_trayVolumeAction = new QWidgetAction(this);
  auto* volumeWidget = new QWidget(m_trayMenu);
  auto* volumeLayout = new QHBoxLayout(volumeWidget);
  volumeLayout->setContentsMargins(10, 6, 10, 6);

  m_trayVolumeLabel = new QLabel(tr("Vol"), volumeWidget);
  m_trayVolumeLabel->setMinimumWidth(40);
  volumeLayout->addWidget(m_trayVolumeLabel);

  m_trayVolumeSlider = new QSlider(Qt::Horizontal, volumeWidget);
  m_trayVolumeSlider->setRange(0, 200);
  m_trayVolumeSlider->setSingleStep(1);
  m_trayVolumeSlider->setPageStep(5);
  m_trayVolumeSlider->setValue(100);
  m_trayVolumeSlider->setToolTip(tr("Output volume (%)"));
  volumeLayout->addWidget(m_trayVolumeSlider, 1);

  connect(m_trayVolumeSlider, &QSlider::valueChanged, this, &MainWindow::applyTrayVolumePercent);

  m_trayVolumeAction->setDefaultWidget(volumeWidget);
  m_trayMenu->addAction(m_trayVolumeAction);

  m_trayMenu->addSeparator();

  m_trayProfilesMenu = m_trayMenu->addMenu(tr("Profiles"));
  connect(m_trayProfilesMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTrayProfilesMenu);

  m_trayMenu->addSeparator();

  auto* quitAction = m_trayMenu->addAction(tr("Quit"));
  connect(quitAction, &QAction::triggered, this, &MainWindow::requestExitFromTray);

  m_tray->setContextMenu(m_trayMenu);

  connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
      showTabFromTray(QStringLiteral("mixer"));
    }
  });

  m_trayRefreshTimer = new QTimer(this);
  m_trayRefreshTimer->setSingleShot(true);
  m_trayRefreshTimer->setInterval(120);
  connect(m_trayRefreshTimer, &QTimer::timeout, this, &MainWindow::refreshTrayUi);

  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &MainWindow::scheduleTrayRefresh);
  }

  refreshTrayUi();
  m_tray->show();
}

uint32_t MainWindow::trayOutputNodeId() const
{
  if (!m_graph) {
    return 0;
  }
  if (const auto def = m_graph->defaultAudioSinkId()) {
    return *def;
  }
  const auto sinks = m_graph->audioSinks();
  if (!sinks.isEmpty()) {
    return sinks.first().id;
  }
  return 0;
}

void MainWindow::scheduleTrayRefresh()
{
  if (m_trayRefreshTimer) {
    m_trayRefreshTimer->start();
  }
}

void MainWindow::refreshTrayUi()
{
  if (!m_tray || !m_graph) {
    return;
  }

  const uint32_t sinkId = trayOutputNodeId();
  const auto controls = (sinkId != 0) ? m_graph->nodeControls(sinkId) : std::nullopt;

  const bool enabled = sinkId != 0 && controls.has_value();
  const bool hasMute = enabled && controls->hasMute;
  const bool hasVolume = enabled && controls->hasVolume;

  const float vol = hasVolume ? controls->volume : 1.0f;
  const int percent = static_cast<int>(std::round(std::clamp(vol, 0.0f, 2.0f) * 100.0f));
  const bool muted = hasMute ? controls->mute : (hasVolume ? (percent <= 0) : false);

  if (m_trayMuteAction) {
    QSignalBlocker b(m_trayMuteAction);
    m_trayMuteAction->setEnabled(enabled && (hasMute || hasVolume));
    m_trayMuteAction->setChecked(muted);
  }

  if (m_trayVolumeSlider) {
    QSignalBlocker b(m_trayVolumeSlider);
    m_trayVolumeSlider->setEnabled(enabled && hasVolume);
    m_trayVolumeSlider->setValue(std::clamp(percent, 0, 200));
  }

  if (m_trayVolumeLabel) {
    m_trayVolumeLabel->setText(tr("%1%").arg(std::clamp(percent, 0, 200), 3));
  }

  QIcon icon = QIcon::fromTheme(QStringLiteral("audio-volume-high"));
  if (muted) {
    icon = QIcon::fromTheme(QStringLiteral("audio-volume-muted"));
  } else if (percent <= 0) {
    icon = QIcon::fromTheme(QStringLiteral("audio-volume-muted"));
  } else if (percent < 35) {
    icon = QIcon::fromTheme(QStringLiteral("audio-volume-low"));
  } else if (percent < 70) {
    icon = QIcon::fromTheme(QStringLiteral("audio-volume-medium"));
  }
  if (icon.isNull()) {
    icon = QIcon(QStringLiteral(":/icons/app.svg"));
  }
  m_tray->setIcon(icon);
  m_tray->setToolTip(tr("Headroom — Output %1%").arg(std::clamp(percent, 0, 200)));
}

void MainWindow::showTabFromTray(const QString& key)
{
  showNormal();
  raise();
  activateWindow();
  selectTabByKey(key);
}

void MainWindow::toggleTrayMute()
{
  if (!m_graph || !m_trayMuteAction) {
    return;
  }
  const uint32_t sinkId = trayOutputNodeId();
  if (sinkId == 0) {
    return;
  }

  const auto controls = m_graph->nodeControls(sinkId);
  const bool hasMute = controls.has_value() && controls->hasMute;
  const bool hasVolume = controls.has_value() && controls->hasVolume;

  const bool mute = m_trayMuteAction->isChecked();
  if (hasMute) {
    m_graph->setNodeMute(sinkId, mute);
    scheduleTrayRefresh();
    return;
  }

  // Software mute fallback for nodes that don't expose a mute control:
  // set volume to 0 and restore it when unmuting.
  if (!hasVolume) {
    scheduleTrayRefresh();
    return;
  }

  if (mute) {
    float restore = controls->volume;
    if (restore <= 0.0001f) {
      restore = 1.0f;
    }
    m_traySoftMuteNodeId = sinkId;
    m_traySoftMuteRestoreVolume = restore;
    m_graph->setNodeVolume(sinkId, 0.0f);
  } else {
    float restore = 1.0f;
    if (m_traySoftMuteNodeId == sinkId && m_traySoftMuteRestoreVolume.has_value()) {
      restore = *m_traySoftMuteRestoreVolume;
    }
    m_traySoftMuteNodeId = 0;
    m_traySoftMuteRestoreVolume.reset();
    m_graph->setNodeVolume(sinkId, restore);
  }

  scheduleTrayRefresh();
}

void MainWindow::applyTrayVolumePercent(int percent)
{
  if (!m_graph) {
    return;
  }
  const uint32_t sinkId = trayOutputNodeId();
  if (sinkId == 0) {
    return;
  }
  const float linear = std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 2.0f);
  m_graph->setNodeVolume(sinkId, linear);
  scheduleTrayRefresh();
}

void MainWindow::rebuildTrayProfilesMenu()
{
  if (!m_trayProfilesMenu) {
    return;
  }

  m_trayProfilesMenu->clear();

  QSettings s;
  const QString current = s.value(SettingsKeys::patchbaySelectedProfileName()).toString();
  const QStringList names = PatchbayProfileStore::listProfileNames(s);

  if (names.isEmpty()) {
    auto* none = m_trayProfilesMenu->addAction(tr("(no profiles)"));
    none->setEnabled(false);
    return;
  }

  for (const auto& name : names) {
    auto* a = m_trayProfilesMenu->addAction(name);
    a->setCheckable(true);
    a->setChecked(name == current);
    connect(a, &QAction::triggered, this, [this, name]() {
      if (!m_graph) {
        return;
      }
      QSettings s;
      const QString prevActive = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();
      const auto profile = PatchbayProfileStore::load(s, name);
      if (!profile) {
        if (m_tray) {
          m_tray->showMessage(tr("Headroom"), tr("Profile \"%1\" could not be loaded.").arg(name), QSystemTrayIcon::Warning);
        }
        return;
      }

      PatchbayProfileHookStartResult unloadHook;
      if (!prevActive.isEmpty() && prevActive != name) {
        const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, prevActive);
        unloadHook = startPatchbayProfileHookDetached(prevActive, QString{}, name, PatchbayProfileHookEvent::Unload, h.onUnloadCommand);
      }

      const PatchbayProfileApplyResult r = applyPatchbayProfile(*m_graph, *profile, false);
      s.setValue(SettingsKeys::patchbaySelectedProfileName(), name);
      s.setValue(SettingsKeys::patchbayActiveProfileName(), name);

      const PatchbayProfileHooks h2 = PatchbayProfileHooksStore::load(s, name);
      const PatchbayProfileHookStartResult loadHook =
          startPatchbayProfileHookDetached(name, prevActive, QString{}, PatchbayProfileHookEvent::Load, h2.onLoadCommand);

      if (m_patchbayPage) {
        m_patchbayPage->refresh();
      }
      if (m_mixerPage) {
        m_mixerPage->refresh();
      }

      QString msg = tr("Applied profile \"%1\".\nCreated %2, already %3, missing %4.")
                              .arg(name)
                              .arg(r.createdLinks)
                              .arg(r.alreadyPresentLinks)
                              .arg(r.missingEndpoints);
      if (unloadHook.started || loadHook.started || !unloadHook.error.isEmpty() || !loadHook.error.isEmpty()) {
        msg += tr("\nHooks:");
        if (unloadHook.started) {
          msg += tr("\n- Unload “%1” (pid %2)").arg(prevActive).arg(unloadHook.pid);
        } else if (!unloadHook.error.isEmpty()) {
          msg += tr("\n- Unload “%1”: %2").arg(prevActive, unloadHook.error);
        }
        if (loadHook.started) {
          msg += tr("\n- Load “%1” (pid %2)").arg(name).arg(loadHook.pid);
        } else if (!loadHook.error.isEmpty()) {
          msg += tr("\n- Load “%1”: %2").arg(name, loadHook.error);
        }
      }
      if (m_tray) {
        m_tray->showMessage(tr("Headroom"), msg, QSystemTrayIcon::Information);
      }
    });
  }
}

void MainWindow::requestExitFromTray()
{
  m_trayExitRequested = true;
  if (m_tray) {
    m_tray->hide();
  }
  qApp->quit();
}
