#include "EngineDialog.h"

#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

#include "backend/EngineControl.h"

namespace {

QString statusText(const SystemdUnitStatus& st)
{
  if (!st.error.isEmpty()) {
    return QObject::tr("error: %1").arg(st.error);
  }
  if (!st.exists()) {
    return QObject::tr("not found");
  }
  const QString a = st.activeState.isEmpty() ? QStringLiteral("unknown") : st.activeState;
  const QString s = st.subState.isEmpty() ? QStringLiteral("unknown") : st.subState;
  return QObject::tr("%1 (%2)").arg(a, s);
}

} // namespace

void EngineDialog::refresh()
{
  const bool systemctlOk = EngineControl::isSystemctlAvailable();
  QString busErr;
  const bool busOk = systemctlOk ? EngineControl::canTalkToUserSystemd(&busErr) : false;

  if (!systemctlOk) {
    m_summaryLabel->setText(tr("systemctl was not found. Engine control requires systemd user units."));
  } else if (!busOk) {
    m_summaryLabel->setText(tr("systemd user instance is unavailable: %1").arg(busErr.isEmpty() ? tr("(unknown)") : busErr));
  } else {
    m_summaryLabel->setText(tr("Controls PipeWire and the session manager via systemd user units."));
  }

  refreshClockUi();
  refreshDiagnosticsUi();
  refreshMidiBridgeUi();

  for (auto& r : m_rows) {
    const bool enabled = systemctlOk && busOk;

    SystemdUnitStatus st;
    if (enabled) {
      st = EngineControl::queryUserUnit(r.unit);
    } else {
      st.unit = r.unit;
      st.loadState = QStringLiteral("not-found");
      st.activeState = QStringLiteral("unknown");
      st.subState = QStringLiteral("unknown");
      if (!systemctlOk) {
        st.error = QStringLiteral("systemctl missing");
      } else if (!busOk) {
        st.error = busErr;
      }
    }

    if (r.statusLabel) {
      r.statusLabel->setText(statusText(st));
    }
    if (r.startButton) {
      r.startButton->setEnabled(enabled && st.exists());
    }
    if (r.stopButton) {
      r.stopButton->setEnabled(enabled && st.exists());
    }
    if (r.restartButton) {
      r.restartButton->setEnabled(enabled && st.exists());
    }
  }
}

void EngineDialog::runAction(const QString& action, const QString& unit)
{
  QString err;
  bool ok = false;
  if (action == QStringLiteral("start")) {
    ok = EngineControl::startUserUnit(unit, &err);
  } else if (action == QStringLiteral("stop")) {
    ok = EngineControl::stopUserUnit(unit, &err);
  } else if (action == QStringLiteral("restart")) {
    ok = EngineControl::restartUserUnit(unit, &err);
  } else {
    return;
  }

  refresh();

  if (!ok) {
    QMessageBox::warning(this, tr("Engine Control"), tr("%1 %2 failed: %3").arg(action, unit, err.isEmpty() ? tr("(unknown)") : err));
  }
}

