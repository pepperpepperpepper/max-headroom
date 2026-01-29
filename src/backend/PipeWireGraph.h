#pragma once

#include <QObject>
#include <QHash>
#include <QString>

#include <atomic>
#include <mutex>
#include <optional>

#include <pipewire/core.h>

class PipeWireThread;

struct PwNodeInfo final {
  uint32_t id = 0;
  QString name;
  QString description;
  QString mediaClass;
  QString appName;
  QString appProcessBinary;
  QString objectSerial;
  uint32_t audioChannels = 0;
  QString audioPosition;
};

struct PwPortInfo final {
  uint32_t id = 0;
  uint32_t nodeId = 0;
  QString name;
  QString direction; // "in" or "out"
  QString audioChannel;
  QString alias;
  QString mediaType;  // Audio/Video/Midi (best-effort)
  QString formatDsp;
  QString objectSerial;
};

struct PwLinkInfo final {
  uint32_t id = 0;
  uint32_t outputNodeId = 0;
  uint32_t outputPortId = 0;
  uint32_t inputNodeId = 0;
  uint32_t inputPortId = 0;
};

struct PwModuleInfo final {
  uint32_t id = 0;
  QString name;
  QString description;
  QString objectSerial;
};

struct PwNodeControls final {
  bool hasVolume = false;
  bool hasMute = false;

  float volume = 1.0f; // average linear volume
  bool mute = false;

  QVector<float> channelVolumes;
};

struct PwClockSettings final {
  std::optional<uint32_t> rate;
  QVector<uint32_t> allowedRates;
  std::optional<uint32_t> quantum;
  std::optional<uint32_t> minQuantum;
  std::optional<uint32_t> maxQuantum;
  std::optional<uint32_t> forceRate;
  std::optional<uint32_t> forceQuantum;
};

struct PwClockPreset final {
  QString id;
  QString title;
  std::optional<uint32_t> forceRate;
  std::optional<uint32_t> forceQuantum;
};

struct PwProfilerBlock final {
  uint32_t id = 0;
  QString name;
  int status = 0;
  int xrunCount = 0;
  std::optional<double> latencyMs;
  std::optional<double> waitMs;
  std::optional<double> busyMs;
  std::optional<double> waitRatio;
  std::optional<double> busyRatio;
};

struct PwProfilerSnapshot final {
  uint64_t seq = 0;

  bool hasInfo = false;
  uint64_t counter = 0;
  double cpuLoadFast = 0.0;
  double cpuLoadMedium = 0.0;
  double cpuLoadSlow = 0.0;
  int xrunCount = 0;

  bool hasClock = false;
  std::optional<double> clockDurationMs;
  std::optional<double> clockDelayMs;
  std::optional<double> clockXrunDurationMs;
  int clockCycle = 0;

  QVector<PwProfilerBlock> drivers;
  QVector<PwProfilerBlock> followers;
};

class PipeWireGraph final : public QObject
{
  Q_OBJECT

public:
  explicit PipeWireGraph(PipeWireThread* pw, QObject* parent = nullptr);
  ~PipeWireGraph() override;

  PipeWireGraph(const PipeWireGraph&) = delete;
  PipeWireGraph& operator=(const PipeWireGraph&) = delete;

  QList<PwNodeInfo> nodes() const;
  QList<PwPortInfo> ports() const;
  QList<PwLinkInfo> links() const;
  QList<PwModuleInfo> modules() const;

  std::optional<PwNodeInfo> nodeById(uint32_t id) const;
  std::optional<PwNodeControls> nodeControls(uint32_t nodeId) const;

  QList<PwNodeInfo> audioSources() const;
  QList<PwNodeInfo> audioSinks() const;
  QList<PwNodeInfo> audioPlaybackStreams() const;
  QList<PwNodeInfo> audioCaptureStreams() const;

  bool hasDefaultDeviceSupport() const;
  std::optional<uint32_t> defaultAudioSinkId() const;
  std::optional<uint32_t> defaultAudioSourceId() const;
  bool setDefaultAudioSink(uint32_t nodeId);
  bool setDefaultAudioSource(uint32_t nodeId);

  bool hasClockSettingsSupport() const;
  PwClockSettings clockSettings() const;

  static QVector<PwClockPreset> clockPresets();
  bool applyClockPreset(const QString& presetId);

  bool setClockForceRate(std::optional<uint32_t> rate);
  bool setClockForceQuantum(std::optional<uint32_t> quantum);
  bool setClockMinQuantum(std::optional<uint32_t> quantum);
  bool setClockMaxQuantum(std::optional<uint32_t> quantum);

  bool hasProfilerSupport() const;
  std::optional<PwProfilerSnapshot> profilerSnapshot() const;

  bool setNodeVolume(uint32_t nodeId, float volume);
  bool setNodeMute(uint32_t nodeId, bool mute);

  bool createLink(uint32_t outputNodeId, uint32_t outputPortId, uint32_t inputNodeId, uint32_t inputPortId);
  bool destroyLink(uint32_t linkId);

signals:
  void graphChanged();
  void topologyChanged();
  void nodeControlsChanged();
  void metadataChanged();

private:
  enum ChangeFlag : uint32_t {
    ChangeTopology = 1U << 0U,
    ChangeNodeControls = 1U << 1U,
    ChangeMetadata = 1U << 2U,
  };

  struct NodeBinding;
  struct MetadataBinding;
  struct ProfilerBinding;

  static void onRegistryGlobal(void* data,
                               uint32_t id,
                               uint32_t permissions,
                               const char* type,
                               uint32_t version,
                               const struct spa_dict* props);
  static void onRegistryGlobalRemove(void* data, uint32_t id);

  static void onNodeInfo(void* data, const struct pw_node_info* info);
  static void onNodeParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod* param);
  static int onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value);
  static void onProfilerProfile(void* data, const struct spa_pod* pod);

  void bindNode(uint32_t id);
  void unbindNode(uint32_t id);
  void bindMetadata(uint32_t id, const QString& name);
  void unbindMetadata(uint32_t id);
  void bindProfiler(uint32_t id);
  void unbindProfiler(uint32_t id);

  void scheduleGraphChanged(uint32_t flags);

  PipeWireThread* m_pw = nullptr;
  pw_registry* m_registry = nullptr;
  spa_hook m_registryListener{};

  mutable std::mutex m_mutex;
  QHash<uint32_t, PwNodeInfo> m_nodes;
  QHash<uint32_t, PwPortInfo> m_ports;
  QHash<uint32_t, PwLinkInfo> m_links;
  QHash<uint32_t, PwModuleInfo> m_modules;
  QHash<uint32_t, PwNodeControls> m_nodeControls;

  QHash<uint32_t, NodeBinding*> m_nodeBindings;
  QHash<uint32_t, MetadataBinding*> m_metadataBindings;
  MetadataBinding* m_defaultDeviceMetadata = nullptr;
  MetadataBinding* m_settingsMetadata = nullptr;
  ProfilerBinding* m_profilerBinding = nullptr;
  std::optional<uint32_t> m_defaultAudioSinkId;
  std::optional<uint32_t> m_configuredAudioSinkId;
  std::optional<uint32_t> m_defaultAudioSourceId;
  std::optional<uint32_t> m_configuredAudioSourceId;

  std::optional<uint32_t> m_clockRate;
  QVector<uint32_t> m_clockAllowedRates;
  std::optional<uint32_t> m_clockQuantum;
  std::optional<uint32_t> m_clockMinQuantum;
  std::optional<uint32_t> m_clockMaxQuantum;
  std::optional<uint32_t> m_clockForceRate;
  std::optional<uint32_t> m_clockForceQuantum;

  std::optional<PwProfilerSnapshot> m_profilerSnapshot;

  // We keep proxies for links we create so the server-side resource stays alive
  // until disconnect; with object.linger=true, links can persist beyond our lifetime.
  QVector<void*> m_createdLinkProxies;

  std::atomic_uint32_t m_pendingChangeFlags{0};
  std::atomic_bool m_emitScheduled{false};
};
