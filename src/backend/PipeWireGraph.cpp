#include "PipeWireGraphInternal.h"

#include <QMetaObject>

#include <pipewire/core.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

PipeWireGraph::PipeWireGraph(PipeWireThread* pw, QObject* parent)
    : QObject(parent)
    , m_pw(pw)
{
  if (!m_pw || !m_pw->isConnected()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  m_registry = pw_core_get_registry(m_pw->core(), PW_VERSION_REGISTRY, 0);
  if (!m_registry) {
    pw_thread_loop_unlock(loop);
    return;
  }

  static const pw_registry_events registryEvents = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = &PipeWireGraph::onRegistryGlobal;
    e.global_remove = &PipeWireGraph::onRegistryGlobalRemove;
    return e;
  }();
  pw_registry_add_listener(m_registry, &m_registryListener, &registryEvents, this);

  // Ensure we get the current global list (not only future changes).
  pw_core_sync(m_pw->core(), PW_ID_CORE, 0);

  pw_thread_loop_unlock(loop);
}

PipeWireGraph::~PipeWireGraph()
{
  if (!m_pw || !m_pw->threadLoop()) {
    return;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  {
    QHash<uint32_t, NodeBinding*> bindings;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      bindings = m_nodeBindings;
      m_nodeBindings.clear();
      m_nodeControls.clear();
    }

    for (auto* b : bindings) {
      if (!b) {
        continue;
      }
      spa_hook_remove(&b->listener);
      if (b->node) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(b->node));
      }
      delete b;
    }
  }

  {
    QHash<uint32_t, MetadataBinding*> bindings;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      bindings = m_metadataBindings;
      m_metadataBindings.clear();
      m_defaultDeviceMetadata = nullptr;
      m_settingsMetadata = nullptr;
      m_defaultAudioSinkId.reset();
      m_configuredAudioSinkId.reset();
      m_defaultAudioSourceId.reset();
      m_configuredAudioSourceId.reset();
      m_clockRate.reset();
      m_clockAllowedRates.clear();
      m_clockQuantum.reset();
      m_clockMinQuantum.reset();
      m_clockMaxQuantum.reset();
      m_clockForceRate.reset();
      m_clockForceQuantum.reset();
    }

    for (auto* b : bindings) {
      if (!b) {
        continue;
      }
      spa_hook_remove(&b->listener);
      if (b->metadata) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(b->metadata));
      }
      delete b;
    }
  }

  {
    ProfilerBinding* binding = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      binding = m_profilerBinding;
      m_profilerBinding = nullptr;
      m_profilerSnapshot.reset();
    }

    if (binding) {
      spa_hook_remove(&binding->listener);
      if (binding->profiler) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(binding->profiler));
      }
      delete binding;
    }
  }

  if (m_registry) {
    spa_hook_remove(&m_registryListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
    m_registry = nullptr;
  }

  pw_thread_loop_unlock(loop);
}

QList<PwNodeInfo> PipeWireGraph::nodes() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_nodes.values();
}

QList<PwPortInfo> PipeWireGraph::ports() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_ports.values();
}

QList<PwLinkInfo> PipeWireGraph::links() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_links.values();
}

QList<PwModuleInfo> PipeWireGraph::modules() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_modules.values();
}

std::optional<PwNodeInfo> PipeWireGraph::nodeById(uint32_t id) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_nodes.contains(id)) {
    return std::nullopt;
  }
  return m_nodes.value(id);
}

std::optional<PwNodeControls> PipeWireGraph::nodeControls(uint32_t nodeId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_nodeControls.contains(nodeId)) {
    return std::nullopt;
  }
  return m_nodeControls.value(nodeId);
}

QList<PwNodeInfo> PipeWireGraph::audioSources() const
{
  QList<PwNodeInfo> out;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      const auto& node = it.value();
      if (node.mediaClass == QStringLiteral("Audio/Source")) {
        out.push_back(node);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioSinks() const
{
  QList<PwNodeInfo> out;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      const auto& node = it.value();
      if (node.mediaClass == QStringLiteral("Audio/Sink")) {
        out.push_back(node);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioPlaybackStreams() const
{
  QList<PwNodeInfo> out;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      const auto& node = it.value();
      if (node.mediaClass.startsWith(QStringLiteral("Stream/Output/Audio"))) {
        out.push_back(node);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

QList<PwNodeInfo> PipeWireGraph::audioCaptureStreams() const
{
  QList<PwNodeInfo> out;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      const auto& node = it.value();
      if (node.mediaClass.startsWith(QStringLiteral("Stream/Input/Audio"))) {
        out.push_back(node);
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return a.description < b.description; });
  return out;
}

bool PipeWireGraph::hasDefaultDeviceSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_defaultDeviceMetadata != nullptr && m_defaultDeviceMetadata->metadata != nullptr;
}

std::optional<uint32_t> PipeWireGraph::defaultAudioSinkId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_defaultAudioSinkId.has_value()) {
    return m_defaultAudioSinkId;
  }
  if (!m_defaultAudioSinkName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_defaultAudioSinkName) {
        return it.value().id;
      }
    }
  }
  if (m_configuredAudioSinkId.has_value()) {
    return m_configuredAudioSinkId;
  }
  if (!m_configuredAudioSinkName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_configuredAudioSinkName) {
        return it.value().id;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> PipeWireGraph::defaultAudioSourceId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_defaultAudioSourceId.has_value()) {
    return m_defaultAudioSourceId;
  }
  if (!m_defaultAudioSourceName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_defaultAudioSourceName) {
        return it.value().id;
      }
    }
  }
  if (m_configuredAudioSourceId.has_value()) {
    return m_configuredAudioSourceId;
  }
  if (!m_configuredAudioSourceName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_configuredAudioSourceName) {
        return it.value().id;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> PipeWireGraph::configuredAudioSinkId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_configuredAudioSinkId.has_value()) {
    return m_configuredAudioSinkId;
  }
  if (!m_configuredAudioSinkName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_configuredAudioSinkName) {
        return it.value().id;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint32_t> PipeWireGraph::configuredAudioSourceId() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_configuredAudioSourceId.has_value()) {
    return m_configuredAudioSourceId;
  }
  if (!m_configuredAudioSourceName.isEmpty()) {
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
      if (it.value().name == m_configuredAudioSourceName) {
        return it.value().id;
      }
    }
  }
  return std::nullopt;
}

void PipeWireGraph::scheduleGraphChanged(uint32_t flags)
{
  static const bool debugRate = (std::getenv("HEADROOM_DEBUG_PW_RATE") != nullptr);
  if (debugRate) {
    static std::atomic<uint64_t> s_scheduleCalls{0};
    static std::atomic<uint64_t> s_scheduleTopology{0};
    static std::atomic<uint64_t> s_scheduleNodeControls{0};
    static std::atomic<uint64_t> s_scheduleMetadata{0};
    static std::atomic<int64_t> s_lastLogNs{0};

    s_scheduleCalls.fetch_add(1, std::memory_order_relaxed);
    if (flags & ChangeTopology) {
      s_scheduleTopology.fetch_add(1, std::memory_order_relaxed);
    }
    if (flags & ChangeNodeControls) {
      s_scheduleNodeControls.fetch_add(1, std::memory_order_relaxed);
    }
    if (flags & ChangeMetadata) {
      s_scheduleMetadata.fetch_add(1, std::memory_order_relaxed);
    }

    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    int64_t expected = s_lastLogNs.load(std::memory_order_relaxed);
    if (expected <= 0 || nowNs - expected >= 1'000'000'000LL) {
      if (s_lastLogNs.compare_exchange_strong(expected, nowNs)) {
        const uint64_t calls = s_scheduleCalls.exchange(0, std::memory_order_relaxed);
        const uint64_t topo = s_scheduleTopology.exchange(0, std::memory_order_relaxed);
        const uint64_t ctrl = s_scheduleNodeControls.exchange(0, std::memory_order_relaxed);
        const uint64_t meta = s_scheduleMetadata.exchange(0, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "headroom: debug: pw scheduleGraphChanged ~1s: calls=%llu topology=%llu nodeControls=%llu metadata=%llu\n",
                     static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(topo),
                     static_cast<unsigned long long>(ctrl),
                     static_cast<unsigned long long>(meta));
      }
    }
  }

  m_pendingChangeFlags.fetch_or(flags, std::memory_order_relaxed);

  bool expected = false;
  if (!m_emitScheduled.compare_exchange_strong(expected, true)) {
    return;
  }

  QMetaObject::invokeMethod(
      this,
      [this]() {
        m_emitScheduled.store(false);
        const uint32_t changeFlags = m_pendingChangeFlags.exchange(0U, std::memory_order_relaxed);

        static const bool debugRate = (std::getenv("HEADROOM_DEBUG_PW_RATE") != nullptr);
        if (debugRate) {
          static std::atomic<uint64_t> s_emitCalls{0};
          static std::atomic<uint64_t> s_emitTopology{0};
          static std::atomic<uint64_t> s_emitNodeControls{0};
          static std::atomic<uint64_t> s_emitMetadata{0};
          static std::atomic<int64_t> s_lastLogNs{0};

          s_emitCalls.fetch_add(1, std::memory_order_relaxed);
          if (changeFlags & ChangeTopology) {
            s_emitTopology.fetch_add(1, std::memory_order_relaxed);
          }
          if (changeFlags & ChangeNodeControls) {
            s_emitNodeControls.fetch_add(1, std::memory_order_relaxed);
          }
          if (changeFlags & ChangeMetadata) {
            s_emitMetadata.fetch_add(1, std::memory_order_relaxed);
          }

          const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
          int64_t expected = s_lastLogNs.load(std::memory_order_relaxed);
          if (expected <= 0 || nowNs - expected >= 1'000'000'000LL) {
            if (s_lastLogNs.compare_exchange_strong(expected, nowNs)) {
              const uint64_t calls = s_emitCalls.exchange(0, std::memory_order_relaxed);
              const uint64_t topo = s_emitTopology.exchange(0, std::memory_order_relaxed);
              const uint64_t ctrl = s_emitNodeControls.exchange(0, std::memory_order_relaxed);
              const uint64_t meta = s_emitMetadata.exchange(0, std::memory_order_relaxed);
              std::fprintf(stderr,
                           "headroom: debug: pw emit graphChanged ~1s: calls=%llu topology=%llu nodeControls=%llu metadata=%llu\n",
                           static_cast<unsigned long long>(calls),
                           static_cast<unsigned long long>(topo),
                           static_cast<unsigned long long>(ctrl),
                           static_cast<unsigned long long>(meta));
            }
          }
        }

        emit graphChanged();
        if (changeFlags & ChangeTopology) {
          emit topologyChanged();
        }
        if (changeFlags & ChangeNodeControls) {
          emit nodeControlsChanged();
        }
        if (changeFlags & ChangeMetadata) {
          emit metadataChanged();
        }
      },
      Qt::QueuedConnection);
}
