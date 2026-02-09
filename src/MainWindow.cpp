#include "MainWindow.h"

#include <QAbstractNativeEventFilter>
#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QCoreApplication>
#include <QEvent>
#include <QHideEvent>
#include <QSettings>
#include <QShowEvent>
#include <QTabWidget>
#include <QToolBar>
#include <QTimer>
#include <QWindow>

#include "backend/AudioRecorder.h"
#include "backend/AudioTap.h"
#include "backend/EqManager.h"
#include "backend/LogStore.h"
#include "backend/PatchbayAutoConnectController.h"
#include "backend/PatchbayPersistentProfileController.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "settings/SettingsKeys.h"
#include "ui/GraphPage.h"
#include "ui/MixerPage.h"
#include "ui/PatchbayPage.h"
#include "ui/VisualizerPage.h"
#include "ui/WindowVisibility.h"

#if QT_CONFIG(xcb)
#include <xcb/xcb.h>
#endif

class MainWindowX11MapEventFilter final : public QAbstractNativeEventFilter
{
public:
  explicit MainWindowX11MapEventFilter(MainWindow* w)
      : m_window(w)
  {
  }

  void setWindowId(WId id) { m_windowId = id; }

  bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
  {
    Q_UNUSED(result);
#if QT_CONFIG(xcb)
    if (!m_window || m_windowId == 0) {
      return false;
    }
    if (!eventType.startsWith("xcb") || !message) {
      return false;
    }

    const auto* ev = static_cast<const xcb_generic_event_t*>(message);
    const uint8_t type = ev->response_type & ~0x80;
    if (type != XCB_MAP_NOTIFY && type != XCB_UNMAP_NOTIFY && type != XCB_DESTROY_NOTIFY) {
      return false;
    }

    xcb_window_t win = 0;
    if (type == XCB_MAP_NOTIFY) {
      win = reinterpret_cast<const xcb_map_notify_event_t*>(ev)->window;
    } else if (type == XCB_UNMAP_NOTIFY) {
      win = reinterpret_cast<const xcb_unmap_notify_event_t*>(ev)->window;
    } else if (type == XCB_DESTROY_NOTIFY) {
      win = reinterpret_cast<const xcb_destroy_notify_event_t*>(ev)->window;
    }

    if (static_cast<WId>(win) == m_windowId) {
      m_window->scheduleLowPowerModeUpdate();
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
#endif
    return false;
  }

private:
  MainWindow* m_window = nullptr;
  WId m_windowId = 0;
};

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
  m_patchbayPersistent = new PatchbayPersistentProfileController(m_graph, this);

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

  if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
    auto f = std::make_unique<MainWindowX11MapEventFilter>(this);
    f->setWindowId(winId());
    m_nativeEventFilter = std::move(f);
    qApp->installNativeEventFilter(m_nativeEventFilter.get());
  }

  // Some X11 environments used for host testing can map/unmap the native window without Qt
  // delivering the expected QWidget show/hide transitions. Initialize and hook low-power mode
  // once the event loop starts, even if showEvent() doesn't fire as expected.
  QTimer::singleShot(0, this, [this]() {
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
#if QT_CONFIG(xcb)
      auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
      xcb_connection_t* conn = x11 ? x11->connection() : nullptr;
      const WId wid = winId();
      if (conn && wid != 0) {
        const xcb_window_t win = static_cast<xcb_window_t>(wid);
        const xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes(conn, win);
        xcb_get_window_attributes_reply_t* reply = xcb_get_window_attributes_reply(conn, cookie, nullptr);
        if (reply) {
          const uint32_t want = reply->your_event_mask | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
          std::free(reply);
          const uint32_t values[] = {want};
          xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, values);
          xcb_flush(conn);
        }
      }
#endif

      if (auto* f = dynamic_cast<MainWindowX11MapEventFilter*>(m_nativeEventFilter.get())) {
        f->setWindowId(winId());
      }
    }
    if (!m_windowHandleHooked) {
      if (QWindow* w = windowHandle()) {
        m_windowHandleHooked = true;
        connect(w, &QWindow::visibilityChanged, this, [this](QWindow::Visibility) { updateLowPowerMode(); });
        connect(w, &QWindow::visibleChanged, this, [this](bool) { updateLowPowerMode(); });
      }
    }
    updateLowPowerMode();
  });

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

MainWindow::~MainWindow()
{
  if (m_nativeEventFilter) {
    qApp->removeNativeEventFilter(m_nativeEventFilter.get());
    m_nativeEventFilter.reset();
  }
}

void MainWindow::showEvent(QShowEvent* event)
{
  QMainWindow::showEvent(event);

  // On some X11 setups, the native window may be mapped/unmapped without Qt delivering the
  // expected QWidget show/hide transitions. Track QWindow visibility changes so low-power
  // behavior (meters/visualizer/profiler) stays in sync with the actual on-screen state.
  if (!m_windowHandleHooked) {
    if (QWindow* w = windowHandle()) {
      m_windowHandleHooked = true;
      connect(w, &QWindow::visibilityChanged, this, [this](QWindow::Visibility) { updateLowPowerMode(); });
      connect(w, &QWindow::visibleChanged, this, [this](bool) { updateLowPowerMode(); });
    }
  }

  updateLowPowerMode();
}

void MainWindow::hideEvent(QHideEvent* event)
{
  QMainWindow::hideEvent(event);
  updateLowPowerMode();
}

void MainWindow::changeEvent(QEvent* event)
{
  QMainWindow::changeEvent(event);
  if (event && event->type() == QEvent::WindowStateChange) {
    updateLowPowerMode();
  }
}

void MainWindow::scheduleLowPowerModeUpdate()
{
  if (m_lowPowerUpdatePending) {
    return;
  }
  m_lowPowerUpdatePending = true;
  QTimer::singleShot(0, this, [this]() {
    m_lowPowerUpdatePending = false;
    updateLowPowerMode();
  });
}

void MainWindow::updateLowPowerMode()
{
  if (!m_graph) {
    return;
  }

  const bool enableProfiler = WindowVisibility::isActive(this) && m_profilerRequested;
  m_graph->setProfilerEnabled(enableProfiler);

  if (m_mixerPage) {
    QTimer::singleShot(0, m_mixerPage, &MixerPage::updateForWindowVisibilityChange);
  }
  if (m_visualizerPage) {
    QTimer::singleShot(0, m_visualizerPage, &VisualizerPage::updateForWindowVisibilityChange);
  }
}

void MainWindow::setVisualizerTapTarget(const QString& targetObject, bool captureSink)
{
  if (m_visualizerPage) {
    m_visualizerPage->setTapTarget(targetObject, captureSink);
  } else if (m_tap) {
    m_tap->setTarget(captureSink, targetObject);
  }
}

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
