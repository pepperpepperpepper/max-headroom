#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>

#include <memory>

#include <pipewire/pipewire.h>

#include "MainWindow.h"
#include "backend/PipeWireGraph.h"
#include "backend/EqConfig.h"
#include "backend/LogStore.h"
#include "ui/EngineDialog.h"
#include "ui/EqDialog.h"
#include "ui/LogsDialog.h"
#include "ui/SettingsDialog.h"

int main(int argc, char** argv)
{
  pw_init(&argc, &argv);

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Headroom"));
  QApplication::setOrganizationName(QStringLiteral("maxheadroom"));
  QApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));

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
  parser.addOption(tabOpt);
  parser.addOption(tapTargetOpt);
  parser.addOption(tapCaptureSinkOpt);
  parser.addOption(screenshotOpt);
  parser.addOption(screenshotWindowOpt);
  parser.addOption(screenshotWaitNodeOpt);
  parser.addOption(screenshotWaitTimeoutOpt);
  parser.addOption(screenshotDelayOpt);
  parser.process(app);

  int ret = 0;
  {
    MainWindow window(logs);
    window.resize(1100, 700);
    window.show();

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
          state->graphConn = QObject::connect(graph, &PipeWireGraph::graphChanged, target, [graphHasMatch, scheduleAfterDelay]() {
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

    ret = app.exec();
  }

  pw_deinit();
  return ret;
}
