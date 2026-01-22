#pragma once

#include <QObject>
#include <QVector>

#include <atomic>
#include <mutex>

#include <pipewire/filter.h>

#include "backend/EqConfig.h"

class PipeWireThread;

class ParametricEqFilter final : public QObject
{
  Q_OBJECT

public:
  struct Channel final {
    QString name; // e.g. "FL"
    uint32_t spaChannel = 0;
  };

  explicit ParametricEqFilter(PipeWireThread* pw,
                              QString nodeName,
                              QString nodeDescription,
                              QVector<Channel> channels,
                              QObject* parent = nullptr);
  ~ParametricEqFilter() override;

  uint32_t nodeId() const { return m_nodeId.load(); }
  QString nodeName() const { return m_nodeName; }
  QVector<Channel> channels() const { return m_channels; }

  EqPreset preset() const;
  void setPreset(const EqPreset& preset);

private:
  struct Biquad final {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
  };

  struct BiquadState final {
    double z1 = 0.0;
    double z2 = 0.0;
  };

  static void onFilterStateChanged(void* data, enum pw_filter_state old, enum pw_filter_state state, const char* error);
  static void onFilterParamChanged(void* data, void* port_data, uint32_t id, const struct spa_pod* param);
  static void onFilterProcess(void* data, struct spa_io_position* position);

  void connectLocked();
  void destroyLocked();
  void rebuildCoefficientsLocked();

  void processLocked();

  static Biquad makeBiquad(EqBandType type, double sampleRate, double freqHz, double q, double gainDb);
  static double clamp(double v, double lo, double hi);

  PipeWireThread* m_pw = nullptr;
  QString m_nodeName;
  QString m_nodeDescription;
  QVector<Channel> m_channels;

  pw_filter* m_filter = nullptr;
  spa_hook m_filterListener{};

  struct PortPair final {
    void* inPort = nullptr;
    void* outPort = nullptr;
  };
  QVector<PortPair> m_ports;

  mutable std::mutex m_mutex;
  EqPreset m_preset;
  double m_sampleRate = 48000.0;

  QVector<QVector<Biquad>> m_biquads;           // [channel][band]
  QVector<QVector<BiquadState>> m_biquadState;  // [channel][band]

  std::atomic<uint32_t> m_nodeId{0};
};

