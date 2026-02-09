#include "PatchbayPersistentProfileController.h"

#include "backend/PatchbayProfiles.h"
#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"

#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kDebounceMs = 300;
constexpr int kCooldownAfterApplyMs = 800;
} // namespace

PatchbayPersistentProfileController::PatchbayPersistentProfileController(PipeWireGraph* graph, QObject* parent)
    : QObject(parent)
    , m_graph(graph)
{
  if (!m_graph) {
    return;
  }

  connect(m_graph, &PipeWireGraph::topologyChanged, this, &PatchbayPersistentProfileController::scheduleApply);

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setTimerType(Qt::CoarseTimer);
  connect(m_timer, &QTimer::timeout, this, &PatchbayPersistentProfileController::apply);

  m_clock.start();
  scheduleApply();
}

void PatchbayPersistentProfileController::applyNow()
{
  apply();
}

void PatchbayPersistentProfileController::scheduleApply()
{
  if (!m_timer || m_applying) {
    return;
  }

  QSettings s;
  const QString active = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();
  if (active.isEmpty()) {
    return;
  }

  const qint64 nowMs = m_clock.isValid() ? m_clock.elapsed() : 0;

  int delayMs = kDebounceMs;
  if (nowMs < m_cooldownUntilMs) {
    const qint64 waitMs = m_cooldownUntilMs - nowMs;
    if (waitMs > 0) {
      delayMs = std::max(delayMs, static_cast<int>(std::min<qint64>(waitMs, 60'000)));
    }
  }
  m_timer->start(delayMs);
}

void PatchbayPersistentProfileController::apply()
{
  if (!m_graph || m_applying) {
    return;
  }

  QSettings s;
  const QString active = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();
  if (active.isEmpty()) {
    return;
  }

  const auto profile = PatchbayProfileStore::load(s, active);
  if (!profile) {
    return;
  }

  m_applying = true;
  const PatchbayProfileApplyResult r = applyPatchbayProfile(*m_graph, *profile, false);
  m_applying = false;

  if (r.createdLinks > 0 || r.disconnectedLinks > 0) {
    const qint64 nowMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    m_cooldownUntilMs = std::max(m_cooldownUntilMs, nowMs + kCooldownAfterApplyMs);
  }
}
