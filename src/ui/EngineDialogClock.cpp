#include "EngineDialog.h"

#include <algorithm>
#include <optional>

#include <QComboBox>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QVariant>

#include "backend/PipeWireGraph.h"

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

