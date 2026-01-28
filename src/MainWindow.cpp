#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QCoreApplication>
#include <QSettings>
#include <QTabWidget>
#include <QToolBar>

#include "backend/AudioRecorder.h"
#include "backend/AudioTap.h"
#include "backend/EqManager.h"
#include "backend/LogStore.h"
#include "backend/PatchbayAutoConnectController.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "settings/SettingsKeys.h"
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
