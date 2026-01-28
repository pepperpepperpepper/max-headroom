#include "PipeWireGraphInternal.h"

#include <QByteArray>
#include <QRegularExpression>

#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/extensions/profiler.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/properties.h>

#include <spa/param/profiler.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <algorithm>

bool PipeWireGraph::setDefaultAudioSink(uint32_t nodeId)
{
  if (nodeId == 0 || !m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_defaultDeviceMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(nodeId);
  const int r1 = pw_metadata_set_property(binding->metadata, 0, "default.audio.sink", "Spa:Id", value.constData());
  const int r2 = pw_metadata_set_property(binding->metadata, 0, "default.configured.audio.sink", "Spa:Id", value.constData());

  pw_thread_loop_unlock(loop);
  return r1 >= 0 && r2 >= 0;
}

bool PipeWireGraph::setDefaultAudioSource(uint32_t nodeId)
{
  if (nodeId == 0 || !m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_defaultDeviceMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(nodeId);
  const int r1 = pw_metadata_set_property(binding->metadata, 0, "default.audio.source", "Spa:Id", value.constData());
  const int r2 = pw_metadata_set_property(binding->metadata, 0, "default.configured.audio.source", "Spa:Id", value.constData());

  pw_thread_loop_unlock(loop);
  return r1 >= 0 && r2 >= 0;
}

bool PipeWireGraph::hasClockSettingsSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_settingsMetadata != nullptr && m_settingsMetadata->metadata != nullptr;
}

PwClockSettings PipeWireGraph::clockSettings() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  PwClockSettings s;
  s.rate = m_clockRate;
  s.allowedRates = m_clockAllowedRates;
  s.quantum = m_clockQuantum;
  s.minQuantum = m_clockMinQuantum;
  s.maxQuantum = m_clockMaxQuantum;
  s.forceRate = m_clockForceRate;
  s.forceQuantum = m_clockForceQuantum;
  return s;
}

QVector<PwClockPreset> PipeWireGraph::clockPresets()
{
  return {
      {QStringLiteral("auto"), tr("Auto (session-managed)"), std::nullopt, std::nullopt},
      {QStringLiteral("ll-48k-64"), tr("Low latency — 48 kHz / 64"), 48000u, 64u},
      {QStringLiteral("ll-48k-128"), tr("Low latency — 48 kHz / 128"), 48000u, 128u},
      {QStringLiteral("balanced-48k-256"), tr("Balanced — 48 kHz / 256"), 48000u, 256u},
      {QStringLiteral("stable-48k-512"), tr("Stable — 48 kHz / 512"), 48000u, 512u},
      {QStringLiteral("hq-96k-256"), tr("High quality — 96 kHz / 256"), 96000u, 256u},
  };
}

bool PipeWireGraph::applyClockPreset(const QString& presetId)
{
  const QString id = presetId.trimmed().toLower();
  const auto presets = clockPresets();
  for (const auto& p : presets) {
    if (p.id == id) {
      const bool r1 = setClockForceRate(p.forceRate);
      const bool r2 = setClockForceQuantum(p.forceQuantum);
      return r1 && r2;
    }
  }
  return false;
}

bool PipeWireGraph::setClockForceRate(std::optional<uint32_t> rate)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(rate.value_or(0));
  const int res = pw_metadata_set_property(binding->metadata, 0, "clock.force-rate", "Spa:Int", value.constData());

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockForceQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = QByteArray::number(quantum.value_or(0));
  const int res = pw_metadata_set_property(binding->metadata, 0, "clock.force-quantum", "Spa:Int", value.constData());

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockMinQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = quantum.has_value() ? QByteArray::number(*quantum) : QByteArray();
  const int res = pw_metadata_set_property(binding->metadata,
                                           0,
                                           "clock.min-quantum",
                                           "Spa:Int",
                                           quantum.has_value() ? value.constData() : nullptr);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::setClockMaxQuantum(std::optional<uint32_t> quantum)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  MetadataBinding* binding = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    binding = m_settingsMetadata;
  }

  if (!binding || !binding->metadata) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const QByteArray value = quantum.has_value() ? QByteArray::number(*quantum) : QByteArray();
  const int res = pw_metadata_set_property(binding->metadata,
                                           0,
                                           "clock.max-quantum",
                                           "Spa:Int",
                                           quantum.has_value() ? value.constData() : nullptr);

  pw_thread_loop_unlock(loop);
  return res >= 0;
}

bool PipeWireGraph::hasProfilerSupport() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_profilerBinding != nullptr && m_profilerBinding->profiler != nullptr;
}

std::optional<PwProfilerSnapshot> PipeWireGraph::profilerSnapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_profilerSnapshot;
}
