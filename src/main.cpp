#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QWindow>

#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <time.h>

#include <pipewire/pipewire.h>

#include "MainWindow.h"
#include "backend/AudioLevelTap.h"
#include "backend/PipeWireGraph.h"
#include "backend/EqConfig.h"
#include "backend/LogStore.h"
#include "settings/SettingsKeys.h"
#include "ui/AppTheme.h"
#include "ui/EngineDialog.h"
#include "ui/EqDialog.h"
#include "ui/LevelMeterWidget.h"
#include "ui/LogsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/WindowVisibility.h"

int main(int argc, char** argv)
{
  pw_init(&argc, &argv);
  struct PipeWireDeinitGuard final {
    ~PipeWireDeinitGuard() { pw_deinit(); }
  } pwGuard;

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Headroom"));
  QApplication::setOrganizationName(QStringLiteral("maxheadroom"));
  QApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
  AppTheme::applyFromSettings();

  auto* logs = new LogStore(&app);
  logs->installQtMessageHandler();
  logs->installPipeWireLogger();

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("Headroom: PipeWire-first mixer + patchbay + visualizers"));
  parser.addHelpOption();
  parser.addVersionOption();

  QCommandLineOption tabOpt(QStringList{QStringLiteral("tab")},
                            QStringLiteral("Select initial tab: mixer|visualizer|patchbay|graph"),
                            QStringLiteral("name"));
  QCommandLineOption tapTargetOpt(QStringList{QStringLiteral("tap-target")},
                                  QStringLiteral("Set initial visualizer tap target (node name or object.serial)"),
                                  QStringLiteral("target"));
  QCommandLineOption tapCaptureSinkOpt(QStringList{QStringLiteral("tap-capture-sink")},
                                       QStringLiteral("When used with --tap-target, capture the target as a sink monitor"));
  QCommandLineOption screenshotOpt(QStringList{QStringLiteral("screenshot")},
                                   QStringLiteral("Write a PNG screenshot to PATH and exit"),
                                   QStringLiteral("path"));
  QCommandLineOption screenshotWindowOpt(QStringList{QStringLiteral("screenshot-window")},
                                         QStringLiteral("Select what to screenshot: main|settings|eq|engine|logs"),
                                         QStringLiteral("name"),
                                         QStringLiteral("main"));
  QCommandLineOption screenshotWaitNodeOpt(
      QStringList{QStringLiteral("screenshot-wait-node")},
      QStringLiteral("Wait for a PipeWire node whose name/description matches REGEX (only affects --screenshot-window=main)"),
      QStringLiteral("regex"));
  QCommandLineOption screenshotWaitTimeoutOpt(QStringList{QStringLiteral("screenshot-wait-timeout-ms")},
                                              QStringLiteral("Max time to wait for --screenshot-wait-node before taking the screenshot (ms)"),
                                              QStringLiteral("ms"),
                                              QStringLiteral("9000"));
  QCommandLineOption screenshotDelayOpt(QStringList{QStringLiteral("screenshot-delay-ms")},
                                        QStringLiteral("Delay before taking screenshot (ms)"),
                                        QStringLiteral("ms"),
                                        QStringLiteral("900"));
  QCommandLineOption selfTestMetersHideOpt(QStringList{QStringLiteral("self-test-meters-hide")},
                                           QStringLiteral("Self-test: ensure Mixer meter streams stop when the main window is hidden"));
  QCommandLineOption selfTestMetersMinimizedOpt(
      QStringList{QStringLiteral("self-test-meters-minimized")},
      QStringLiteral("Self-test: ensure Mixer meter streams stop when the main window is minimized"));
  QCommandLineOption selfTestMetersVisibleOpt(QStringList{QStringLiteral("self-test-meters-visible")},
                                              QStringLiteral("Self-test: ensure Mixer meters are active and not repainting excessively while visible"));
  QCommandLineOption selfTestVisualizerHideOpt(
      QStringList{QStringLiteral("self-test-visualizer-hide")},
      QStringLiteral("Self-test: ensure Visualizer tap stream stops when the main window is hidden"));
  QCommandLineOption selfTestVisualizerMinimizedOpt(
      QStringList{QStringLiteral("self-test-visualizer-minimized")},
      QStringLiteral("Self-test: ensure Visualizer tap stream stops when the main window is minimized"));
  parser.addOption(tabOpt);
  parser.addOption(tapTargetOpt);
  parser.addOption(tapCaptureSinkOpt);
  parser.addOption(screenshotOpt);
  parser.addOption(screenshotWindowOpt);
  parser.addOption(screenshotWaitNodeOpt);
  parser.addOption(screenshotWaitTimeoutOpt);
  parser.addOption(screenshotDelayOpt);
  parser.addOption(selfTestMetersHideOpt);
  parser.addOption(selfTestMetersMinimizedOpt);
  parser.addOption(selfTestMetersVisibleOpt);
  parser.addOption(selfTestVisualizerHideOpt);
  parser.addOption(selfTestVisualizerMinimizedOpt);
  parser.process(app);

  const bool wantSelfTestHide = parser.isSet(selfTestMetersHideOpt);
  const bool wantSelfTestMetersMinimized = parser.isSet(selfTestMetersMinimizedOpt);
  const bool wantSelfTestVisible = parser.isSet(selfTestMetersVisibleOpt);
  const bool wantSelfTestVisualizerHide = parser.isSet(selfTestVisualizerHideOpt);
  const bool wantSelfTestVisualizerMinimized = parser.isSet(selfTestVisualizerMinimizedOpt);

  const int selfTestCount = (wantSelfTestHide ? 1 : 0) + (wantSelfTestMetersMinimized ? 1 : 0) + (wantSelfTestVisible ? 1 : 0)
      + (wantSelfTestVisualizerHide ? 1 : 0) + (wantSelfTestVisualizerMinimized ? 1 : 0);
  if (selfTestCount > 1) {
    std::fprintf(stderr,
                 "headroom: self-test flags are mutually exclusive: "
                 "--self-test-meters-hide, --self-test-meters-minimized, --self-test-meters-visible, "
                 "--self-test-visualizer-hide, --self-test-visualizer-minimized\n");
    return 2;
  }

  const bool wantSelfTestMeters = wantSelfTestHide || wantSelfTestMetersMinimized || wantSelfTestVisible;
  const bool wantSelfTest = wantSelfTestMeters || wantSelfTestVisualizerHide || wantSelfTestVisualizerMinimized;
  if (parser.isSet(screenshotOpt) && wantSelfTest) {
    std::fprintf(stderr,
                 "headroom: --screenshot cannot be combined with "
                 "--self-test-meters-hide, --self-test-meters-minimized, --self-test-meters-visible, "
                 "--self-test-visualizer-hide, or --self-test-visualizer-minimized\n");
    return 2;
  }

  QVariant prevMeterMode;
  bool hadPrevMeterMode = false;
  if (wantSelfTestMeters) {
    QSettings s;
    const QString key = SettingsKeys::mixerMetersMode();
    hadPrevMeterMode = s.contains(key);
    prevMeterMode = s.value(key);
    // Ensure meters are enabled so the test actually exercises stream teardown on hide.
    s.setValue(key, 3);
  }

  int ret = 0;
  {
    MainWindow window(logs);
    window.resize(1100, 700);
    window.show();

    // Some real-host X11 test environments have been observed to start the main window in an
    // unmapped/withdrawn state (even after show()). This breaks visibility-dependent features
    // (meters/visualizer) and can skew power measurements. Ensure the window is mapped shortly
    // after the event loop begins, but do not fight intentional hide/minimize states.
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
      auto ensureMapped = [&window]() {
        if (std::getenv("HEADROOM_DEBUG_VISIBILITY")) {
          QWindow* handle = window.windowHandle();
          std::fprintf(
              stderr,
              "headroom: debug: ensureMapped isVisible=%d windowState=0x%x WA_Mapped=%d handleVis=%d active=%d\n",
              window.isVisible() ? 1 : 0,
              static_cast<unsigned int>(window.windowState()),
              window.testAttribute(Qt::WA_Mapped) ? 1 : 0,
              handle ? static_cast<int>(handle->visibility()) : -1,
              WindowVisibility::isActive(&window) ? 1 : 0);
        }
        if (window.windowState() & Qt::WindowMinimized) {
          return;
        }
        if (WindowVisibility::isActive(&window)) {
          return;
        }
        // Force a map/show to recover from environments where the native window starts withdrawn.
        (void)WindowVisibility::requestMap(&window);
        window.showNormal();
        window.raise();
        window.activateWindow();
      };

      QTimer::singleShot(200, &window, ensureMapped);
      QTimer::singleShot(800, &window, ensureMapped);
      QTimer::singleShot(1500, &window, ensureMapped);
    }

    if (parser.isSet(tabOpt)) {
      window.selectTabByKey(parser.value(tabOpt));
    }

    if (parser.isSet(tapTargetOpt)) {
      const QString target = parser.value(tapTargetOpt).trimmed();
      if (!target.isEmpty() && target.toLower() != QStringLiteral("auto")) {
        window.setVisualizerTapTarget(target, parser.isSet(tapCaptureSinkOpt));
      }
    }

    if (parser.isSet(screenshotOpt)) {
      bool ok = false;
      const int delayMs = parser.value(screenshotDelayOpt).toInt(&ok);
      const int d = ok ? std::max(0, delayMs) : 900;
      const QString path = parser.value(screenshotOpt);

      QWidget* target = &window;

      const QString which = parser.value(screenshotWindowOpt).trimmed().toLower();
      const bool allowGraphWait = which == QStringLiteral("main");
      if (which == QStringLiteral("settings")) {
        auto* dlg = new SettingsDialog(window.graph(), &window);
        dlg->show();
        target = dlg;
      } else if (which == QStringLiteral("eq")) {
        EqPreset p = defaultEqPreset(6);
        p.enabled = true;
        auto* dlg = new EqDialog(QStringLiteral("Device"), p, &window);
        dlg->show();
        target = dlg;
      } else if (which == QStringLiteral("engine")) {
        auto* dlg = new EngineDialog(window.graph(), &window);
        dlg->show();
        target = dlg;
      } else if (which == QStringLiteral("logs")) {
        auto* dlg = new LogsDialog(logs, &window);
        dlg->show();
        target = dlg;
      }

      struct ScreenshotState final {
        bool scheduled = false;
        QMetaObject::Connection graphConn;
      };
      auto state = std::make_shared<ScreenshotState>();

      auto takeScreenshot = [state, &app, target, path]() {
        (void)state;
        const qreal dpr = target->devicePixelRatioF();
        const QSize size = target->size() * dpr;

        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        target->render(&painter);
        painter.end();

        image.save(path);
        app.quit();
      };

      auto scheduleAfterDelay = [state, d, target, takeScreenshot]() {
        if (state->scheduled) {
          return;
        }
        state->scheduled = true;
        if (state->graphConn) {
          QObject::disconnect(state->graphConn);
          state->graphConn = {};
        }
        QTimer::singleShot(d, target, takeScreenshot);
      };

      const QString waitReStr = parser.value(screenshotWaitNodeOpt).trimmed();
      if (allowGraphWait && !waitReStr.isEmpty() && window.graph()) {
        QRegularExpression waitRe(waitReStr);
        const bool valid = waitRe.isValid();

        PipeWireGraph* graph = window.graph();
        auto graphHasMatch = [graph, valid, waitRe]() -> bool {
          if (!valid || !graph) {
            return false;
          }
          const QList<PwNodeInfo> nodes = graph->nodes();
          for (const auto& n : nodes) {
            if (waitRe.match(n.name).hasMatch() || waitRe.match(n.description).hasMatch()) {
              return true;
            }
          }
          return false;
        };

        if (valid && graphHasMatch()) {
          scheduleAfterDelay();
        } else {
          state->graphConn = QObject::connect(graph, &PipeWireGraph::topologyChanged, target, [graphHasMatch, scheduleAfterDelay]() {
            if (graphHasMatch()) {
              scheduleAfterDelay();
            }
          });

          bool okTimeout = false;
          const int timeoutMs = parser.value(screenshotWaitTimeoutOpt).toInt(&okTimeout);
          const int tmo = okTimeout ? std::max(0, timeoutMs) : 9000;
          if (tmo > 0) {
            QTimer::singleShot(tmo, target, scheduleAfterDelay);
          }
        }
      } else {
        scheduleAfterDelay();
      }
    }

    if (wantSelfTest) {
      if (wantSelfTestVisualizerHide || wantSelfTestVisualizerMinimized) {
        window.selectTabByKey(QStringLiteral("visualizer"));
      } else {
        window.selectTabByKey(QStringLiteral("mixer"));
      }
      PipeWireGraph* graph = window.graph();
      if (!graph) {
        std::fprintf(stderr, "headroom: self-test: PipeWire graph unavailable\n");
        app.exit(1);
      } else {
        auto cpuTimeNs = []() -> std::int64_t {
          timespec ts{};
          if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
            return 0;
          }
          return (static_cast<std::int64_t>(ts.tv_sec) * 1000'000'000LL) + static_cast<std::int64_t>(ts.tv_nsec);
        };

        auto countMeterNodes = [graph]() -> int {
          int count = 0;
          const QList<PwNodeInfo> nodes = graph->nodes();
          for (const auto& n : nodes) {
            if (n.name.startsWith(QStringLiteral("headroom.meter."))) {
              ++count;
            }
          }
          return count;
        };

        auto countVisualizerNodes = [graph]() -> int {
          int count = 0;
          const QList<PwNodeInfo> nodes = graph->nodes();
          for (const auto& n : nodes) {
            if (n.name == QStringLiteral("headroom.visualizer")) {
              ++count;
            }
          }
          return count;
        };

        auto hasAudioTargetNodes = [graph]() -> bool {
          const QList<PwNodeInfo> nodes = graph->nodes();
          for (const auto& n : nodes) {
            if (n.name.startsWith(QStringLiteral("headroom."))) {
              continue;
            }
            if (n.mediaClass.contains(QStringLiteral("Audio"))) {
              return true;
            }
          }
          return false;
        };

        if (wantSelfTestHide || wantSelfTestMetersMinimized) {
          const bool minimizeInsteadOfHide = wantSelfTestMetersMinimized;
          enum class Phase {
            WaitForAudioNodes,
            WaitForMetersPresent,
            HideOrMinimizeWindow,
            WaitForMetersGone,
            MeasureIdleCpu,
          };

          struct SelfTestState final {
            Phase phase = Phase::WaitForAudioNodes;
            QElapsedTimer started;
            QElapsedTimer cpuWindow;
            std::int64_t cpuStartNs = 0;
            bool sawMeters = false;
            bool stateIssued = false;
          };
          auto state = std::make_shared<SelfTestState>();
          state->started.start();

          auto* timer = new QTimer(&window);
          timer->setInterval(50);
          timer->setTimerType(Qt::CoarseTimer);
          QObject::connect(timer,
                           &QTimer::timeout,
                           &window,
                           [timer, state, &app, &window, minimizeInsteadOfHide, cpuTimeNs, countMeterNodes, hasAudioTargetNodes]() {
            constexpr int kTimeoutMs = 12000;
            if (state->started.isValid() && state->started.elapsed() > kTimeoutMs) {
              timer->stop();
              std::fprintf(stderr, "headroom: self-test: timeout (phase=%d, sawMeters=%d)\n", static_cast<int>(state->phase), state->sawMeters ? 1 : 0);
              app.exit(1);
              return;
            }

            const int metersNow = countMeterNodes();
            state->sawMeters = state->sawMeters || (metersNow > 0);

            switch (state->phase) {
              case Phase::WaitForAudioNodes: {
                if (hasAudioTargetNodes()) {
                  state->phase = Phase::WaitForMetersPresent;
                }
                break;
              }
              case Phase::WaitForMetersPresent: {
                if (metersNow > 0) {
                  state->phase = Phase::HideOrMinimizeWindow;
                }
                break;
              }
              case Phase::HideOrMinimizeWindow: {
                if (!state->stateIssued) {
                  state->stateIssued = true;
                  if (minimizeInsteadOfHide) {
                    window.showMinimized();
                  } else {
                    window.hide();
                  }
                }
                state->phase = Phase::WaitForMetersGone;
                break;
              }
              case Phase::WaitForMetersGone: {
                if (metersNow == 0) {
                  if (!state->sawMeters) {
                    std::fprintf(stderr, "headroom: self-test: expected to observe meter streams while visible\n");
                    app.exit(1);
                  } else {
                    timer->stop();
                    state->phase = Phase::MeasureIdleCpu;
                    state->cpuStartNs = cpuTimeNs();
                    state->cpuWindow.start();
                    QTimer::singleShot(
                        1200,
                        Qt::CoarseTimer,
                        &window,
                        [state, &app, countMeterNodes, cpuTimeNs]() {
                          const int metersEnd = countMeterNodes();
                          if (metersEnd != 0) {
                            std::fprintf(stderr, "headroom: self-test: expected 0 meter streams while hidden (have %d)\n", metersEnd);
                            app.exit(1);
                            return;
                          }

                          const std::int64_t cpuEndNs = cpuTimeNs();
                          const std::int64_t wallMs = state->cpuWindow.isValid() ? state->cpuWindow.elapsed() : 0;
                          const std::int64_t cpuDeltaNs = (cpuEndNs > state->cpuStartNs) ? (cpuEndNs - state->cpuStartNs) : 0;

                          if (wallMs <= 0) {
                            app.exit(0);
                            return;
                          }

                          const double cpuPct = (100.0 * static_cast<double>(cpuDeltaNs)) / (static_cast<double>(wallMs) * 1e6);
                          constexpr double kMaxCpuPct = 10.0;
                          if (cpuPct > kMaxCpuPct) {
                            std::fprintf(stderr, "headroom: self-test: CPU too high while hidden: %.2f%% (max %.2f%%)\n", cpuPct, kMaxCpuPct);
                            app.exit(1);
                          } else {
                            app.exit(0);
                          }
                        });
                  }
                }
                break;
              }
              case Phase::MeasureIdleCpu: {
                break;
              }
            }
          });
          timer->start();
        } else if (wantSelfTestVisible) {
          struct PaintFilter final : QObject {
            explicit PaintFilter(QObject* parent = nullptr)
                : QObject(parent)
            {
            }

            int meterPaints = 0;
            int windowPaints = 0;
            QObject* window = nullptr;

            bool eventFilter(QObject* watched, QEvent* event) override
            {
              if (event && event->type() == QEvent::Paint) {
                if (qobject_cast<LevelMeterWidget*>(watched)) {
                  ++meterPaints;
                }
                if (window && watched == window) {
                  ++windowPaints;
                }
              }
              return false;
            }
          };

          enum class Phase {
            WaitForAudioNodes,
            WaitForMetersPresent,
            Measure,
          };

          struct SelfTestState final {
            Phase phase = Phase::WaitForAudioNodes;
            QElapsedTimer started;
            int paintStart = 0;
            int windowPaintStart = 0;
            int heartbeat = 0;
            int heartbeatStart = 0;
          };

          auto state = std::make_shared<SelfTestState>();
          state->started.start();

          auto* paintFilter = new PaintFilter(&window);
          paintFilter->window = &window;
          app.installEventFilter(paintFilter);

          auto* heartbeatTimer = new QTimer(&window);
          heartbeatTimer->setInterval(50);
          heartbeatTimer->setTimerType(Qt::CoarseTimer);
          QObject::connect(heartbeatTimer, &QTimer::timeout, &window, [state]() { ++state->heartbeat; });

          auto* timer = new QTimer(&window);
          timer->setInterval(50);
          timer->setTimerType(Qt::CoarseTimer);
          QObject::connect(timer, &QTimer::timeout, &window, [timer, state, &app, &window, paintFilter, heartbeatTimer, countMeterNodes, hasAudioTargetNodes]() {
            constexpr int kTimeoutMs = 12000;
            if (state->started.isValid() && state->started.elapsed() > kTimeoutMs) {
              timer->stop();
              std::fprintf(stderr, "headroom: self-test: timeout (phase=%d)\n", static_cast<int>(state->phase));
              app.exit(1);
              return;
            }

            switch (state->phase) {
              case Phase::WaitForAudioNodes: {
                if (hasAudioTargetNodes()) {
                  state->phase = Phase::WaitForMetersPresent;
                }
                break;
              }
              case Phase::WaitForMetersPresent: {
                const int metersNow = countMeterNodes();
                if (metersNow > 0) {
                  timer->stop();
                  state->phase = Phase::Measure;

                  state->paintStart = paintFilter->meterPaints;
                  state->windowPaintStart = paintFilter->windowPaints;
                  state->heartbeatStart = state->heartbeat;

                  heartbeatTimer->start();

                  constexpr int kMeasureMs = 2000;
                  QTimer::singleShot(
                      kMeasureMs,
                      Qt::CoarseTimer,
                      &window,
                      [state, &app, &window, paintFilter, heartbeatTimer, countMeterNodes]() {
                        heartbeatTimer->stop();

                        const int metersEnd = countMeterNodes();
                        if (metersEnd <= 0) {
                          std::fprintf(stderr, "headroom: self-test: expected meter streams while visible\n");
                          app.exit(1);
                          return;
                        }

                        const QList<AudioLevelTap*> taps = window.findChildren<AudioLevelTap*>();
                        int enabledTapCount = 0;
                        for (auto* t : taps) {
                          if (t && t->isEnabled()) {
                            ++enabledTapCount;
                          }
                        }

                        if (enabledTapCount <= 0) {
                          std::fprintf(stderr, "headroom: self-test: expected active meter taps while visible\n");
                          app.exit(1);
                          return;
                        }

                        const int paintDelta = paintFilter->meterPaints - state->paintStart;
                        const int heartbeatDelta = state->heartbeat - state->heartbeatStart;

                        constexpr int kMeasureMs = 2000;
                        constexpr int kMeterIntervalMs = 33;
                        const int expectedTicks = std::max(1, (kMeasureMs + (kMeterIntervalMs - 1)) / kMeterIntervalMs);
                        const int maxPaints = (enabledTapCount * expectedTicks * 4) + 200;
                        if (paintDelta > maxPaints) {
                          std::fprintf(stderr, "headroom: self-test: excessive repaint while visible: %d paints (max %d)\n", paintDelta, maxPaints);
                          app.exit(1);
                          return;
                        }

                        if (heartbeatDelta < 15) {
                          std::fprintf(stderr, "headroom: self-test: UI unresponsive while visible (heartbeat=%d)\n", heartbeatDelta);
                          app.exit(1);
                          return;
                        }

                        app.exit(0);
                      });
                }
                break;
              }
              case Phase::Measure: {
                break;
              }
            }
          });
          timer->start();
        } else if (wantSelfTestVisualizerHide || wantSelfTestVisualizerMinimized) {
          const bool minimizeInsteadOfHide = wantSelfTestVisualizerMinimized;
          enum class Phase {
            WaitForAudioNodes,
            WaitForVisualizerPresent,
            HideOrMinimizeWindow,
            WaitForVisualizerGone,
            MeasureIdleCpu,
          };

          struct SelfTestState final {
            Phase phase = Phase::WaitForAudioNodes;
            QElapsedTimer started;
            QElapsedTimer cpuWindow;
            std::int64_t cpuStartNs = 0;
            bool sawVisualizer = false;
            bool stateIssued = false;
          };
          auto state = std::make_shared<SelfTestState>();
          state->started.start();

          auto* timer = new QTimer(&window);
          timer->setInterval(50);
          timer->setTimerType(Qt::CoarseTimer);
          QObject::connect(timer,
                           &QTimer::timeout,
                           &window,
                           [timer, state, &app, &window, minimizeInsteadOfHide, cpuTimeNs, countVisualizerNodes, hasAudioTargetNodes]() {
            constexpr int kTimeoutMs = 12000;
            if (state->started.isValid() && state->started.elapsed() > kTimeoutMs) {
              timer->stop();
              std::fprintf(stderr, "headroom: self-test: timeout (phase=%d, sawVisualizer=%d)\n", static_cast<int>(state->phase), state->sawVisualizer ? 1 : 0);
              app.exit(1);
              return;
            }

            const int visNow = countVisualizerNodes();
            state->sawVisualizer = state->sawVisualizer || (visNow > 0);

            switch (state->phase) {
              case Phase::WaitForAudioNodes: {
                if (hasAudioTargetNodes()) {
                  state->phase = Phase::WaitForVisualizerPresent;
                }
                break;
              }
              case Phase::WaitForVisualizerPresent: {
                if (visNow > 0) {
                  state->phase = Phase::HideOrMinimizeWindow;
                }
                break;
              }
              case Phase::HideOrMinimizeWindow: {
                if (!state->stateIssued) {
                  state->stateIssued = true;
                  if (minimizeInsteadOfHide) {
                    window.showMinimized();
                  } else {
                    window.hide();
                  }
                }
                state->phase = Phase::WaitForVisualizerGone;
                break;
              }
              case Phase::WaitForVisualizerGone: {
                if (visNow == 0) {
                  if (!state->sawVisualizer) {
                    std::fprintf(stderr, "headroom: self-test: expected to observe visualizer tap stream while visible\n");
                    app.exit(1);
                  } else {
                    timer->stop();
                    state->phase = Phase::MeasureIdleCpu;
                    state->cpuStartNs = cpuTimeNs();
                    state->cpuWindow.start();
                    QTimer::singleShot(
                        2000,
                        Qt::CoarseTimer,
                        &window,
                        [state, &app, countVisualizerNodes, cpuTimeNs]() {
                          const int visEnd = countVisualizerNodes();
                          if (visEnd != 0) {
                            std::fprintf(stderr, "headroom: self-test: expected 0 visualizer tap streams while hidden (have %d)\n", visEnd);
                            app.exit(1);
                            return;
                          }

                          const std::int64_t cpuEndNs = cpuTimeNs();
                          const std::int64_t wallMs = state->cpuWindow.isValid() ? state->cpuWindow.elapsed() : 0;
                          const std::int64_t cpuDeltaNs = (cpuEndNs > state->cpuStartNs) ? (cpuEndNs - state->cpuStartNs) : 0;

                          if (wallMs <= 0) {
                            app.exit(0);
                            return;
                          }

                          const double cpuPct = (100.0 * static_cast<double>(cpuDeltaNs)) / (static_cast<double>(wallMs) * 1e6);
                          constexpr double kMaxCpuPct = 2.0;
                          if (cpuPct > kMaxCpuPct) {
                            std::fprintf(stderr, "headroom: self-test: CPU too high while hidden: %.2f%% (max %.2f%%)\n", cpuPct, kMaxCpuPct);
                            app.exit(1);
                          } else {
                            app.exit(0);
                          }
                        });
                  }
                }
                break;
              }
              case Phase::MeasureIdleCpu: {
                break;
              }
            }
          });
          timer->start();
        }
      }
    }

    ret = app.exec();
  }

  if (wantSelfTestMeters) {
    QSettings s;
    const QString key = SettingsKeys::mixerMetersMode();
    if (hadPrevMeterMode) {
      s.setValue(key, prevMeterMode);
    } else {
      s.remove(key);
    }
  }
  return ret;
}
