#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include <pipewire/pipewire.h>

#include "MainWindow.h"
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
  QCommandLineOption screenshotDelayOpt(QStringList{QStringLiteral("screenshot-delay-ms")},
                                        QStringLiteral("Delay before taking screenshot (ms)"),
                                        QStringLiteral("ms"),
                                        QStringLiteral("900"));
  parser.addOption(tabOpt);
  parser.addOption(tapTargetOpt);
  parser.addOption(tapCaptureSinkOpt);
  parser.addOption(screenshotOpt);
  parser.addOption(screenshotWindowOpt);
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

      QTimer::singleShot(d, target, [&app, target, path]() {
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
      });
    }

    ret = app.exec();
  }

  pw_deinit();
  return ret;
}
