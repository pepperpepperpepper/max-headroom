#include "EngineControl.h"

#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace {

QString trimLine(QString s)
{
  s = s.trimmed();
  if (s.endsWith(QLatin1Char('\n'))) {
    s.chop(1);
  }
  return s.trimmed();
}

QString firstNonEmptyLine(const QString& text)
{
  const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const auto& l : lines) {
    const QString t = l.trimmed();
    if (!t.isEmpty()) {
      return t;
    }
  }
  return {};
}

} // namespace

bool EngineControl::isSystemctlAvailable()
{
  return !QStandardPaths::findExecutable(QStringLiteral("systemctl")).isEmpty();
}

bool EngineControl::runSystemctl(const QStringList& args, QString* stdoutText, QString* stderrText, int* exitCode)
{
  if (!isSystemctlAvailable()) {
    if (stderrText) {
      *stderrText = QStringLiteral("systemctl not found");
    }
    if (exitCode) {
      *exitCode = 127;
    }
    return false;
  }

  QProcess p;
  p.setProgram(QStringLiteral("systemctl"));
  p.setArguments(args);
  p.setProcessChannelMode(QProcess::SeparateChannels);
  p.start();

  if (!p.waitForStarted(2000)) {
    if (stderrText) {
      *stderrText = QStringLiteral("failed to start systemctl");
    }
    if (exitCode) {
      *exitCode = 1;
    }
    return false;
  }

  if (!p.waitForFinished(7000)) {
    p.kill();
    p.waitForFinished(1000);
    if (stderrText) {
      *stderrText = QStringLiteral("systemctl timed out");
    }
    if (exitCode) {
      *exitCode = 124;
    }
    return false;
  }

  if (stdoutText) {
    *stdoutText = QString::fromLocal8Bit(p.readAllStandardOutput());
  }
  if (stderrText) {
    *stderrText = QString::fromLocal8Bit(p.readAllStandardError());
  }
  if (exitCode) {
    *exitCode = p.exitCode();
  }
  return p.exitStatus() == QProcess::NormalExit;
}

bool EngineControl::canTalkToUserSystemd(QString* error)
{
  if (!isSystemctlAvailable()) {
    if (error) {
      *error = QStringLiteral("systemctl not found");
    }
    return false;
  }

  QString out;
  QString err;
  int code = 0;
  const bool ok = runSystemctl({QStringLiteral("--user"), QStringLiteral("show-environment")}, &out, &err, &code);
  if (!ok || code != 0) {
    if (error) {
      const QString msg = firstNonEmptyLine(err.isEmpty() ? out : err);
      *error = msg.isEmpty() ? QStringLiteral("failed to connect to systemd user instance") : msg;
    }
    return false;
  }
  return true;
}

QStringList EngineControl::defaultUserUnits()
{
  return QStringList{
      QStringLiteral("pipewire.service"),
      QStringLiteral("pipewire-pulse.service"),
      QStringLiteral("wireplumber.service"),
  };
}

QString EngineControl::normalizeUnitAlias(const QString& unitOrAlias)
{
  const QString s = unitOrAlias.trimmed().toLower();
  if (s.isEmpty()) {
    return {};
  }

  if (s == QStringLiteral("pipewire") || s == QStringLiteral("pw")) {
    return QStringLiteral("pipewire.service");
  }
  if (s == QStringLiteral("pipewire-pulse") || s == QStringLiteral("pulse") || s == QStringLiteral("pw-pulse")) {
    return QStringLiteral("pipewire-pulse.service");
  }
  if (s == QStringLiteral("wireplumber") || s == QStringLiteral("wp")) {
    return QStringLiteral("wireplumber.service");
  }

  if (s.endsWith(QStringLiteral(".service")) || s.endsWith(QStringLiteral(".socket")) || s.endsWith(QStringLiteral(".target"))) {
    return s;
  }

  return s + QStringLiteral(".service");
}

SystemdUnitStatus EngineControl::queryUserUnit(const QString& unitOrAlias, QString* error)
{
  SystemdUnitStatus st;
  st.unit = normalizeUnitAlias(unitOrAlias);
  if (st.unit.isEmpty()) {
    st.error = QStringLiteral("invalid unit");
    if (error) {
      *error = st.error;
    }
    return st;
  }

  QString out;
  QString err;
  int code = 0;
  const bool ok = runSystemctl(
      {QStringLiteral("--user"),
       QStringLiteral("show"),
       QStringLiteral("--no-page"),
       QStringLiteral("--property=LoadState"),
       QStringLiteral("--property=ActiveState"),
       QStringLiteral("--property=SubState"),
       QStringLiteral("--property=Description"),
       st.unit},
      &out,
      &err,
      &code);

  if (!ok) {
    st.error = err.isEmpty() ? QStringLiteral("systemctl failed") : firstNonEmptyLine(err);
    if (error) {
      *error = st.error;
    }
    return st;
  }

  auto parseProp = [&](const QString& key) -> QString {
    const QString needle = key + QLatin1Char('=');
    for (const auto& line : out.split(QLatin1Char('\n'))) {
      if (line.startsWith(needle)) {
        return trimLine(line.mid(needle.size()));
      }
    }
    return {};
  };

  st.loadState = parseProp(QStringLiteral("LoadState"));
  st.activeState = parseProp(QStringLiteral("ActiveState"));
  st.subState = parseProp(QStringLiteral("SubState"));
  st.description = parseProp(QStringLiteral("Description"));

  if (code != 0 && st.loadState.isEmpty() && !err.isEmpty()) {
    st.error = firstNonEmptyLine(err);
  }

  if (error && !st.error.isEmpty()) {
    *error = st.error;
  }
  return st;
}

bool EngineControl::startUserUnit(const QString& unitOrAlias, QString* error)
{
  const QString unit = normalizeUnitAlias(unitOrAlias);
  QString out;
  QString err;
  int code = 0;
  const bool ok = runSystemctl({QStringLiteral("--user"), QStringLiteral("start"), unit}, &out, &err, &code);
  if (!ok || code != 0) {
    if (error) {
      *error = firstNonEmptyLine(err.isEmpty() ? out : err);
    }
    return false;
  }
  return true;
}

bool EngineControl::stopUserUnit(const QString& unitOrAlias, QString* error)
{
  const QString unit = normalizeUnitAlias(unitOrAlias);
  QString out;
  QString err;
  int code = 0;
  const bool ok = runSystemctl({QStringLiteral("--user"), QStringLiteral("stop"), unit}, &out, &err, &code);
  if (!ok || code != 0) {
    if (error) {
      *error = firstNonEmptyLine(err.isEmpty() ? out : err);
    }
    return false;
  }
  return true;
}

bool EngineControl::restartUserUnit(const QString& unitOrAlias, QString* error)
{
  const QString unit = normalizeUnitAlias(unitOrAlias);
  QString out;
  QString err;
  int code = 0;
  const bool ok = runSystemctl({QStringLiteral("--user"), QStringLiteral("restart"), unit}, &out, &err, &code);
  if (!ok || code != 0) {
    if (error) {
      *error = firstNonEmptyLine(err.isEmpty() ? out : err);
    }
    return false;
  }
  return true;
}

