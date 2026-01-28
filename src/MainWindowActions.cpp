#include "MainWindow.h"

#include <QSettings>

#include "backend/AudioTap.h"
#include "backend/EqManager.h"
#include "settings/VisualizerSettings.h"
#include "ui/AppTheme.h"
#include "ui/EngineDialog.h"
#include "ui/LogsDialog.h"
#include "ui/MixerPage.h"
#include "ui/PatchbayPage.h"
#include "ui/RecorderDialog.h"
#include "ui/SessionsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/VisualizerPage.h"

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

