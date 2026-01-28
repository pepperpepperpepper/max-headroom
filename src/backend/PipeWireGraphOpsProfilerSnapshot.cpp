#include "PipeWireGraphInternal.h"

#include <pipewire/extensions/profiler.h>

#include <spa/param/profiler.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <algorithm>

static QVector<const struct spa_pod*> collectStructItems(const struct spa_pod* pod)
{
  QVector<const struct spa_pod*> items;
  if (!pod || SPA_POD_TYPE(pod) != SPA_TYPE_Struct) {
    return items;
  }
  const auto* s = reinterpret_cast<const struct spa_pod_struct*>(pod);
  const void* it = nullptr;
  SPA_POD_STRUCT_FOREACH(s, it)
  {
    if (it) {
      items.push_back(reinterpret_cast<const struct spa_pod*>(it));
    }
  }
  return items;
}

static std::optional<double> fractionToMs(const struct spa_fraction& f)
{
  if (f.denom == 0) {
    return std::nullopt;
  }
  return (static_cast<double>(f.num) * 1000.0) / static_cast<double>(f.denom);
}

static void parseProfilerInfo(const struct spa_pod* pod, PwProfilerSnapshot& out)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);

  auto parseFlat = [&](const QVector<const struct spa_pod*>& e, int offset, bool withXruns) -> bool {
    if (e.size() < offset + (withXruns ? 5 : 4)) {
      return false;
    }
    int64_t counter = 0;
    float fast = 0.0f;
    float medium = 0.0f;
    float slow = 0.0f;
    int32_t xruns = 0;

    if (spa_pod_get_long(e[offset + 0], &counter) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 1], &fast) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 2], &medium) != 0) {
      return false;
    }
    if (spa_pod_get_float(e[offset + 3], &slow) != 0) {
      return false;
    }
    if (withXruns) {
      if (spa_pod_get_int(e[offset + 4], &xruns) != 0) {
        return false;
      }
    }

    out.hasInfo = true;
    out.counter = static_cast<uint64_t>(std::max<int64_t>(0, counter));
    out.cpuLoadFast = static_cast<double>(fast);
    out.cpuLoadMedium = static_cast<double>(medium);
    out.cpuLoadSlow = static_cast<double>(slow);
    if (withXruns) {
      out.xrunCount = static_cast<int>(xruns);
    }
    return true;
  };

  // Most common: Struct(Long counter, Float fast, Float medium, Float slow, Int xrunCount)
  if (parseFlat(items, 0, true)) {
    return;
  }

  // Some versions may nest the cpu loads in a Struct and then append xrun count.
  // Struct(Struct(Long, Float, Float, Float), Int xrunCount)
  if (items.size() >= 2 && SPA_POD_TYPE(items[0]) == SPA_TYPE_Struct) {
    int32_t xruns = 0;
    if (spa_pod_get_int(items[1], &xruns) == 0) {
      const QVector<const struct spa_pod*> nested = collectStructItems(items[0]);
      if (parseFlat(nested, 0, false)) {
        out.xrunCount = static_cast<int>(xruns);
        return;
      }
    }
  }
}

static void parseProfilerClock(const struct spa_pod* pod, PwProfilerSnapshot& out)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);
  if (items.size() < 13) {
    return;
  }

  int32_t cycle = 0;
  int64_t duration = 0;
  int64_t delay = 0;
  int64_t xrunDuration = 0;

  // indices per spa/param/profiler.h
  const bool okDuration = (spa_pod_get_long(items[6], &duration) == 0);
  const bool okDelay = (spa_pod_get_long(items[7], &delay) == 0);
  const bool okCycle = (spa_pod_get_int(items[11], &cycle) == 0);
  const bool okXrun = (spa_pod_get_long(items[12], &xrunDuration) == 0);

  out.hasClock = okDuration || okDelay || okCycle || okXrun;
  if (okDuration) {
    out.clockDurationMs = static_cast<double>(duration) / 1'000'000.0;
  }
  if (okDelay) {
    out.clockDelayMs = static_cast<double>(delay) / 1'000'000.0;
  }
  if (okXrun) {
    out.clockXrunDurationMs = static_cast<double>(xrunDuration) / 1'000'000.0;
  }
  if (okCycle) {
    out.clockCycle = static_cast<int>(cycle);
  }
}

static std::optional<PwProfilerBlock> parseProfilerBlock(const struct spa_pod* pod)
{
  const QVector<const struct spa_pod*> items = collectStructItems(pod);
  if (items.size() < 9) {
    return std::nullopt;
  }

  int32_t id = 0;
  const char* name = nullptr;
  int64_t prevSignal = 0;
  int64_t signal = 0;
  int64_t awake = 0;
  int64_t finish = 0;
  int32_t status = 0;
  struct spa_fraction latency{};
  int32_t xruns = 0;

  if (spa_pod_get_int(items[0], &id) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_string(items[1], &name) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[2], &prevSignal) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[3], &signal) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[4], &awake) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_long(items[5], &finish) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_int(items[6], &status) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_fraction(items[7], &latency) != 0) {
    return std::nullopt;
  }
  if (spa_pod_get_int(items[8], &xruns) != 0) {
    return std::nullopt;
  }

  PwProfilerBlock b;
  b.id = static_cast<uint32_t>(std::max(0, id));
  b.name = QString::fromUtf8(name ? name : "");
  b.status = static_cast<int>(status);
  b.xrunCount = static_cast<int>(xruns);
  b.latencyMs = fractionToMs(latency);

  const int64_t periodNs = signal - prevSignal;
  const int64_t waitNs = awake - signal;
  const int64_t busyNs = finish - awake;

  if (periodNs > 0) {
    if (waitNs >= 0) {
      b.waitMs = static_cast<double>(waitNs) / 1'000'000.0;
      b.waitRatio = static_cast<double>(waitNs) / static_cast<double>(periodNs);
    }
    if (busyNs >= 0) {
      b.busyMs = static_cast<double>(busyNs) / 1'000'000.0;
      b.busyRatio = static_cast<double>(busyNs) / static_cast<double>(periodNs);
    }
  }

  return b;
}

void PipeWireGraph::onProfilerProfile(void* data, const struct spa_pod* pod)
{
  auto* binding = static_cast<ProfilerBinding*>(data);
  if (!binding || !binding->graph || !pod) {
    return;
  }

  if (SPA_POD_TYPE(pod) != SPA_TYPE_Object) {
    return;
  }

  PwProfilerSnapshot snap;

  const auto* obj = reinterpret_cast<const struct spa_pod_object*>(pod);
  const struct spa_pod_prop* prop = nullptr;
  SPA_POD_OBJECT_FOREACH(obj, prop)
  {
    if (!prop) {
      continue;
    }
    const struct spa_pod* v = &prop->value;
    switch (prop->key) {
    case SPA_PROFILER_info:
      parseProfilerInfo(v, snap);
      break;
    case SPA_PROFILER_clock:
      parseProfilerClock(v, snap);
      break;
    case SPA_PROFILER_driverBlock: {
      const auto b = parseProfilerBlock(v);
      if (b.has_value()) {
        snap.drivers.push_back(*b);
      }
      break;
    }
    case SPA_PROFILER_followerBlock: {
      const auto b = parseProfilerBlock(v);
      if (b.has_value()) {
        snap.followers.push_back(*b);
      }
      break;
    }
    default:
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(binding->graph->m_mutex);
    const uint64_t nextSeq = binding->graph->m_profilerSnapshot.has_value() ? (binding->graph->m_profilerSnapshot->seq + 1) : 1;
    snap.seq = nextSeq;
    binding->graph->m_profilerSnapshot = snap;
  }
}
