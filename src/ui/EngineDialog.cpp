#include "EngineDialog.h"

#include <algorithm>
#include <cmath>

#include <QDialogButtonBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "backend/AlsaSeqBridge.h"
#include "backend/EngineControl.h"
#include "backend/PipeWireGraph.h"

namespace {

QString prettyUnitName(const QString& unit)
{
  if (unit == QStringLiteral("pipewire.service")) {
    return QObject::tr("PipeWire");
  }
  if (unit == QStringLiteral("pipewire-pulse.service")) {
    return QObject::tr("PipeWire Pulse (PulseAudio compat)");
  }
  if (unit == QStringLiteral("wireplumber.service")) {
    return QObject::tr("WirePlumber (session manager)");
  }
  return unit;
}

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

EngineDialog::EngineDialog(PipeWireGraph* graph, QWidget* parent)
    : QDialog(parent)
    , m_graph(graph)
{
  setWindowTitle(tr("Engine Control"));
  setModal(true);
  resize(760, 460);

  rebuildUi();
  refresh();

  if (m_graph) {
    connect(m_graph, &PipeWireGraph::graphChanged, this, &EngineDialog::refreshClockUi);
    connect(m_graph, &PipeWireGraph::graphChanged, this, &EngineDialog::refreshDiagnosticsUi);
    connect(m_graph, &PipeWireGraph::graphChanged, this, &EngineDialog::refreshMidiBridgeUi);

    m_diagTimer = new QTimer(this);
    m_diagTimer->setInterval(250);
    connect(m_diagTimer, &QTimer::timeout, this, &EngineDialog::refreshDiagnosticsUi);
    m_diagTimer->start();
  }
}

EngineDialog::~EngineDialog() = default;

void EngineDialog::rebuildUi()
{
  auto* root = new QVBoxLayout(this);

  m_summaryLabel = new QLabel(this);
  m_summaryLabel->setWordWrap(true);
  root->addWidget(m_summaryLabel);

  auto* clockBox = new QGroupBox(tr("PipeWire clock (latency)"), this);
  auto* clockGrid = new QGridLayout(clockBox);

  m_clockStatusLabel = new QLabel(tr("…"), clockBox);
  m_clockStatusLabel->setWordWrap(true);

  clockGrid->addWidget(new QLabel(tr("Current"), clockBox), 0, 0);
  clockGrid->addWidget(m_clockStatusLabel, 0, 1, 1, 3);

  m_clockPresetCombo = new QComboBox(clockBox);
  const auto presets = PipeWireGraph::clockPresets();
  for (const auto& p : presets) {
    m_clockPresetCombo->addItem(p.title, p.id);
  }

  m_clockPresetApply = new QPushButton(tr("Apply preset"), clockBox);
  connect(m_clockPresetApply, &QPushButton::clicked, this, &EngineDialog::applyClockPreset);

  clockGrid->addWidget(new QLabel(tr("Preset"), clockBox), 1, 0);
  clockGrid->addWidget(m_clockPresetCombo, 1, 1, 1, 2);
  clockGrid->addWidget(m_clockPresetApply, 1, 3);

  m_forceRateCombo = new QComboBox(clockBox);
  m_forceQuantumCombo = new QComboBox(clockBox);

  m_clockApply = new QPushButton(tr("Apply"), clockBox);
  connect(m_clockApply, &QPushButton::clicked, this, &EngineDialog::applyClockOverrides);

  m_clockReset = new QPushButton(tr("Reset (auto)"), clockBox);
  connect(m_clockReset, &QPushButton::clicked, this, [this]() {
    if (m_forceRateCombo) {
      m_forceRateCombo->setCurrentIndex(0);
    }
    if (m_forceQuantumCombo) {
      m_forceQuantumCombo->setCurrentIndex(0);
    }
    applyClockOverrides();
  });

  clockGrid->addWidget(new QLabel(tr("Force rate"), clockBox), 2, 0);
  clockGrid->addWidget(m_forceRateCombo, 2, 1);
  clockGrid->addWidget(new QLabel(tr("Force quantum"), clockBox), 2, 2);
  clockGrid->addWidget(m_forceQuantumCombo, 2, 3);

  auto* clockActions = new QWidget(clockBox);
  auto* clockActionsLayout = new QHBoxLayout(clockActions);
  clockActionsLayout->setContentsMargins(0, 0, 0, 0);
  clockActionsLayout->addStretch(1);
  clockActionsLayout->addWidget(m_clockReset);
  clockActionsLayout->addWidget(m_clockApply);

  clockGrid->addWidget(clockActions, 3, 0, 1, 4);

  root->addWidget(clockBox);

  auto* diagBox = new QGroupBox(tr("Status / diagnostics"), this);
  auto* diagGrid = new QGridLayout(diagBox);

  m_diagStatusLabel = new QLabel(tr("…"), diagBox);
  m_diagStatusLabel->setWordWrap(true);
  diagGrid->addWidget(new QLabel(tr("Summary"), diagBox), 0, 0);
  diagGrid->addWidget(m_diagStatusLabel, 0, 1);

  m_diagDriversLabel = new QLabel(tr("…"), diagBox);
  m_diagDriversLabel->setWordWrap(true);
  diagGrid->addWidget(new QLabel(tr("Drivers"), diagBox), 1, 0);
  diagGrid->addWidget(m_diagDriversLabel, 1, 1);

  diagGrid->setColumnStretch(1, 1);
  root->addWidget(diagBox);

  auto* midiBox = new QGroupBox(tr("MIDI (ALSA sequencer bridge)"), this);
  auto* midiGrid = new QGridLayout(midiBox);

  m_midiBridgeStatusLabel = new QLabel(tr("…"), midiBox);
  m_midiBridgeStatusLabel->setWordWrap(true);
  midiGrid->addWidget(new QLabel(tr("Status"), midiBox), 0, 0);
  midiGrid->addWidget(m_midiBridgeStatusLabel, 0, 1, 1, 3);

  m_midiBridgeEnableButton = new QPushButton(tr("Enable"), midiBox);
  m_midiBridgeDisableButton = new QPushButton(tr("Disable"), midiBox);

  connect(m_midiBridgeEnableButton, &QPushButton::clicked, this, [this]() {
    QString err;
    if (!AlsaSeqBridge::installConfig(&err)) {
      QMessageBox::warning(this, tr("MIDI Bridge"), tr("Failed to enable ALSA sequencer bridge: %1").arg(err.isEmpty() ? tr("(unknown)") : err));
    }
    refreshMidiBridgeUi();
  });

  connect(m_midiBridgeDisableButton, &QPushButton::clicked, this, [this]() {
    QString err;
    if (!AlsaSeqBridge::removeConfig(&err)) {
      QMessageBox::warning(this, tr("MIDI Bridge"), tr("Failed to disable ALSA sequencer bridge: %1").arg(err.isEmpty() ? tr("(unknown)") : err));
    }
    refreshMidiBridgeUi();
  });

  auto* midiActions = new QWidget(midiBox);
  auto* midiActionsLayout = new QHBoxLayout(midiActions);
  midiActionsLayout->setContentsMargins(0, 0, 0, 0);
  midiActionsLayout->addStretch(1);
  midiActionsLayout->addWidget(m_midiBridgeEnableButton);
  midiActionsLayout->addWidget(m_midiBridgeDisableButton);
  midiGrid->addWidget(midiActions, 1, 0, 1, 4);

  midiGrid->setColumnStretch(1, 1);
  root->addWidget(midiBox);

  auto* box = new QGroupBox(tr("systemd user units"), this);
  auto* grid = new QGridLayout(box);

  grid->addWidget(new QLabel(tr("Service"), box), 0, 0);
  grid->addWidget(new QLabel(tr("Status"), box), 0, 1);
  grid->addWidget(new QLabel(tr("Actions"), box), 0, 2);

  const QStringList units = EngineControl::defaultUserUnits();
  int row = 1;
  m_rows.clear();
  m_rows.reserve(units.size());

  for (const auto& u : units) {
    Row r;
    r.unit = u;
    r.nameLabel = new QLabel(prettyUnitName(u), box);
    r.statusLabel = new QLabel(tr("…"), box);

    auto* actionRow = new QWidget(box);
    auto* actionLayout = new QHBoxLayout(actionRow);
    actionLayout->setContentsMargins(0, 0, 0, 0);

    r.startButton = new QPushButton(tr("Start"), actionRow);
    r.stopButton = new QPushButton(tr("Stop"), actionRow);
    r.restartButton = new QPushButton(tr("Restart"), actionRow);

    actionLayout->addWidget(r.startButton);
    actionLayout->addWidget(r.stopButton);
    actionLayout->addWidget(r.restartButton);
    actionLayout->addStretch(1);

    connect(r.startButton, &QPushButton::clicked, this, [this, u]() { runAction(QStringLiteral("start"), u); });
    connect(r.stopButton, &QPushButton::clicked, this, [this, u]() { runAction(QStringLiteral("stop"), u); });
    connect(r.restartButton, &QPushButton::clicked, this, [this, u]() { runAction(QStringLiteral("restart"), u); });

    grid->addWidget(r.nameLabel, row, 0);
    grid->addWidget(r.statusLabel, row, 1);
    grid->addWidget(actionRow, row, 2);

    m_rows.push_back(r);
    ++row;
  }

  root->addWidget(box, 1);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  m_refreshButton = new QPushButton(tr("Refresh"), this);
  buttons->addButton(m_refreshButton, QDialogButtonBox::ActionRole);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_refreshButton, &QPushButton::clicked, this, &EngineDialog::refresh);

  root->addWidget(buttons);
}

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

void EngineDialog::refreshDiagnosticsUi()
{
  if (!m_diagStatusLabel || !m_diagDriversLabel) {
    return;
  }

  if (!m_graph || !m_graph->hasProfilerSupport()) {
    m_diagStatusLabel->setText(tr("Profiler unavailable (PipeWire module-profiler not loaded)."));
    m_diagDriversLabel->setText(tr("—"));
    return;
  }

  const auto snapOpt = m_graph->profilerSnapshot();
  if (!snapOpt.has_value() || snapOpt->seq == 0) {
    m_diagStatusLabel->setText(tr("Waiting for profiler data…"));
    m_diagDriversLabel->setText(tr("—"));
    return;
  }

  const PwProfilerSnapshot s = *snapOpt;

  auto fmtPct = [](double ratio) -> QString {
    const double pct = ratio * 100.0;
    if (!std::isfinite(pct)) {
      return QObject::tr("—");
    }
    return QObject::tr("%1%").arg(QString::number(pct, 'f', pct < 10.0 ? 2 : 1));
  };

  auto fmtMs = [](const std::optional<double>& ms) -> QString {
    if (!ms.has_value() || !std::isfinite(*ms)) {
      return QObject::tr("—");
    }
    return QObject::tr("%1 ms").arg(QString::number(*ms, 'f', (*ms < 10.0) ? 2 : 1));
  };

  QStringList summary;
  if (s.hasInfo) {
    summary << tr("CPU load (fast/med/slow): %1 / %2 / %3").arg(fmtPct(s.cpuLoadFast), fmtPct(s.cpuLoadMedium), fmtPct(s.cpuLoadSlow));
    summary << tr("XRuns: %1").arg(s.xrunCount);
  } else {
    summary << tr("Profiler info unavailable.");
  }

  if (s.hasClock) {
    QStringList clockBits;
    if (s.clockCycle > 0) {
      clockBits << tr("cycle %1").arg(s.clockCycle);
    }
    if (s.clockDurationMs.has_value()) {
      clockBits << tr("duration %1").arg(fmtMs(s.clockDurationMs));
    }
    if (s.clockDelayMs.has_value()) {
      clockBits << tr("delay %1").arg(fmtMs(s.clockDelayMs));
    }
    if (s.clockXrunDurationMs.has_value() && *s.clockXrunDurationMs > 0.0) {
      clockBits << tr("last xrun %1").arg(fmtMs(s.clockXrunDurationMs));
    }
    if (!clockBits.isEmpty()) {
      summary << tr("Clock: %1").arg(clockBits.join(QStringLiteral(", ")));
    }
  }

  m_diagStatusLabel->setText(summary.join(QStringLiteral("\n")));

  if (s.drivers.isEmpty()) {
    m_diagDriversLabel->setText(tr("(no drivers reported)"));
    return;
  }

  QStringList lines;
  for (const auto& d : s.drivers) {
    QStringList bits;
    bits << tr("%1  %2").arg(d.id).arg(d.name.isEmpty() ? tr("(unnamed)") : d.name);
    bits << tr("lat %1").arg(fmtMs(d.latencyMs));
    if (d.busyRatio.has_value()) {
      bits << tr("busy %1").arg(fmtPct(*d.busyRatio));
    }
    if (d.waitRatio.has_value()) {
      bits << tr("wait %1").arg(fmtPct(*d.waitRatio));
    }
    bits << tr("xruns %1").arg(d.xrunCount);
    lines << bits.join(QStringLiteral("  "));
  }
  m_diagDriversLabel->setText(lines.join(QStringLiteral("\n")));
}

void EngineDialog::refreshMidiBridgeUi()
{
  if (!m_midiBridgeStatusLabel) {
    return;
  }

  const bool enabled = AlsaSeqBridge::isConfigInstalled();
  const bool alsaPresent = AlsaSeqBridge::alsaSequencerDevicePresent();

  bool moduleKnown = false;
  bool moduleLoaded = false;
  if (m_graph) {
    const QList<PwModuleInfo> mods = m_graph->modules();
    moduleKnown = !mods.isEmpty();
    for (const auto& m : mods) {
      if (m.name.toLower().contains(QStringLiteral("alsa-seq"))) {
        moduleLoaded = true;
        break;
      }
    }
  }

  QStringList lines;
  lines << tr("Config: %1").arg(enabled ? tr("enabled (user config snippet)") : tr("disabled"));
  lines << tr("Path: %1").arg(AlsaSeqBridge::configSnippetPath());
  lines << tr("ALSA sequencer device: %1").arg(alsaPresent ? tr("present") : tr("missing (/dev/snd/seq not found)"));
  if (moduleKnown) {
    lines << tr("PipeWire module: %1").arg(moduleLoaded ? tr("loaded") : tr("not loaded"));
  } else {
    lines << tr("PipeWire module: (unknown)");
  }
  if (enabled && moduleKnown && !moduleLoaded) {
    lines << tr("Restart PipeWire to apply changes.");
  }

  m_midiBridgeStatusLabel->setText(lines.join(QStringLiteral("\n")));

  if (m_midiBridgeEnableButton) {
    m_midiBridgeEnableButton->setEnabled(!enabled);
  }
  if (m_midiBridgeDisableButton) {
    m_midiBridgeDisableButton->setEnabled(enabled);
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

void EngineDialog::refreshClockUi()
{
  const bool ok = m_graph && m_graph->hasClockSettingsSupport();
  if (!ok) {
    if (m_clockStatusLabel) {
      m_clockStatusLabel->setText(tr("Clock controls unavailable (no PipeWire settings metadata)."));
    }
    if (m_clockPresetCombo) {
      m_clockPresetCombo->setEnabled(false);
    }
    if (m_clockPresetApply) {
      m_clockPresetApply->setEnabled(false);
    }
    if (m_forceRateCombo) {
      m_forceRateCombo->setEnabled(false);
    }
    if (m_forceQuantumCombo) {
      m_forceQuantumCombo->setEnabled(false);
    }
    if (m_clockApply) {
      m_clockApply->setEnabled(false);
    }
    if (m_clockReset) {
      m_clockReset->setEnabled(false);
    }
    return;
  }

  const PwClockSettings s = m_graph->clockSettings();
  const uint32_t rate = s.rate.value_or(0);
  const uint32_t quantum = s.quantum.value_or(0);
  const uint32_t effRate = s.forceRate.value_or(rate);
  const uint32_t effQuantum = s.forceQuantum.value_or(quantum);

  auto fmtMs = [](uint32_t q, uint32_t r) -> QString {
    if (q == 0 || r == 0) {
      return QObject::tr("—");
    }
    const double ms = (static_cast<double>(q) * 1000.0) / static_cast<double>(r);
    return QObject::tr("%1 ms").arg(QString::number(ms, 'f', ms < 10.0 ? 2 : 1));
  };

  QStringList parts;
  if (rate > 0 && quantum > 0) {
    parts << tr("Current: %1 Hz / %2 frames (%3)").arg(rate).arg(quantum).arg(fmtMs(quantum, rate));
  } else {
    parts << tr("Current: (unknown)");
  }
  if (s.forceRate.has_value() || s.forceQuantum.has_value()) {
    parts << tr("Forced: %1 Hz / %2 frames (%3)")
                 .arg(effRate > 0 ? QString::number(effRate) : tr("auto"))
                 .arg(effQuantum > 0 ? QString::number(effQuantum) : tr("auto"))
                 .arg(fmtMs(effQuantum, effRate));
  }

  if (s.minQuantum.has_value() || s.maxQuantum.has_value()) {
    parts << tr("Limits: %1–%2")
                 .arg(s.minQuantum.has_value() ? QString::number(*s.minQuantum) : tr("?"))
                 .arg(s.maxQuantum.has_value() ? QString::number(*s.maxQuantum) : tr("?"));
  }

  if (m_clockStatusLabel) {
    m_clockStatusLabel->setText(parts.join(QStringLiteral("\n")));
  }

  auto refillCombo = [](QComboBox* combo, const QList<QPair<QString, uint32_t>>& items, uint32_t selected) {
    if (!combo) {
      return;
    }
    const QVariant prev = combo->currentData();
    combo->blockSignals(true);
    combo->clear();
    for (const auto& it : items) {
      combo->addItem(it.first, static_cast<uint32_t>(it.second));
    }
    int target = -1;
    for (int i = 0; i < combo->count(); ++i) {
      if (combo->itemData(i).toUInt() == selected) {
        target = i;
        break;
      }
    }
    if (target < 0 && prev.isValid()) {
      for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i) == prev) {
          target = i;
          break;
        }
      }
    }
    combo->setCurrentIndex(std::max(0, target));
    combo->blockSignals(false);
  };

  QList<QPair<QString, uint32_t>> rateItems;
  rateItems.push_back({tr("Auto"), 0});
  if (!s.allowedRates.isEmpty()) {
    for (uint32_t r : s.allowedRates) {
      rateItems.push_back({tr("%1 Hz").arg(r), r});
    }
  } else {
    for (uint32_t r : {44100u, 48000u, 88200u, 96000u, 192000u}) {
      rateItems.push_back({tr("%1 Hz").arg(r), r});
    }
  }

  QList<QPair<QString, uint32_t>> quantumItems;
  quantumItems.push_back({tr("Auto"), 0});
  const uint32_t minQ = s.minQuantum.value_or(32);
  const uint32_t maxQ = s.maxQuantum.value_or(2048);
  for (uint32_t q : {32u, 48u, 64u, 96u, 128u, 192u, 256u, 384u, 512u, 768u, 1024u, 1536u, 2048u, 4096u}) {
    if (q < minQ || q > maxQ) {
      continue;
    }
    quantumItems.push_back({tr("%1 frames").arg(q), q});
  }

  refillCombo(m_forceRateCombo, rateItems, s.forceRate.value_or(0));
  refillCombo(m_forceQuantumCombo, quantumItems, s.forceQuantum.value_or(0));

  if (m_clockPresetCombo) {
    m_clockPresetCombo->setEnabled(true);
  }
  if (m_clockPresetApply) {
    m_clockPresetApply->setEnabled(true);
  }
  if (m_forceRateCombo) {
    m_forceRateCombo->setEnabled(true);
  }
  if (m_forceQuantumCombo) {
    m_forceQuantumCombo->setEnabled(true);
  }
  if (m_clockApply) {
    m_clockApply->setEnabled(true);
  }
  if (m_clockReset) {
    m_clockReset->setEnabled(true);
  }
}

void EngineDialog::applyClockPreset()
{
  if (!m_graph || !m_clockPresetCombo) {
    return;
  }
  const QString presetId = m_clockPresetCombo->currentData().toString();
  if (presetId.isEmpty()) {
    return;
  }

  const bool ok = m_graph->applyClockPreset(presetId);
  refreshClockUi();

  if (!ok) {
    QMessageBox::warning(this, tr("Engine Control"), tr("Failed to apply clock preset."));
  }
}

void EngineDialog::applyClockOverrides()
{
  if (!m_graph) {
    return;
  }

  const uint32_t rate = m_forceRateCombo ? m_forceRateCombo->currentData().toUInt() : 0;
  const uint32_t quantum = m_forceQuantumCombo ? m_forceQuantumCombo->currentData().toUInt() : 0;

  const bool ok1 = m_graph->setClockForceRate(rate > 0 ? std::optional<uint32_t>(rate) : std::nullopt);
  const bool ok2 = m_graph->setClockForceQuantum(quantum > 0 ? std::optional<uint32_t>(quantum) : std::nullopt);
  refreshClockUi();

  if (!ok1 || !ok2) {
    QMessageBox::warning(this, tr("Engine Control"), tr("Failed to apply clock settings."));
  }
}
