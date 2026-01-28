#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>

#include <pipewire/pipewire.h>

#include "cli/CliInternal.h"

int main(int argc, char** argv)
{
  pw_init(&argc, &argv);

  int exitCode = 0;
  {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Headroom"));
    QCoreApplication::setOrganizationName(QStringLiteral("maxheadroom"));
    QCoreApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));

    QTextStream out(stdout);
    QTextStream err(stderr);

    exitCode = headroomctl::runCommand(app, app.arguments(), out, err);
  }

  pw_deinit();
  return exitCode;
}

