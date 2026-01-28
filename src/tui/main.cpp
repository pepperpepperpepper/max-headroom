#include <QCoreApplication>
#include <QStringList>

#include <pipewire/pipewire.h>

#include <locale.h>

#include "tui/TuiInternal.h"

int main(int argc, char** argv)
{
  setlocale(LC_ALL, "");
  pw_init(&argc, &argv);

  int exitCode = 0;
  {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Headroom"));
    QCoreApplication::setOrganizationName(QStringLiteral("maxheadroom"));
    QCoreApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));

    exitCode = headroomtui::runTui(app, app.arguments());
  }

  pw_deinit();
  return exitCode;
}
