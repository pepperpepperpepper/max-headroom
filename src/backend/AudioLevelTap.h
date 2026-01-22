#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>

#include <pipewire/stream.h>

class PipeWireThread;

class AudioLevelTap final : public QObject
{
  Q_OBJECT

public:
  explicit AudioLevelTap(PipeWireThread* pw, QObject* parent = nullptr);
  ~AudioLevelTap() override;

  AudioLevelTap(const AudioLevelTap&) = delete;
  AudioLevelTap& operator=(const AudioLevelTap&) = delete;

  QString targetObject() const { return m_targetObject; }
  void setTargetObject(const QString& targetObject);

  bool captureSink() const { return m_captureSink.load(std::memory_order_relaxed); }
  void setCaptureSink(bool captureSink);

  float peak() const { return m_peak.load(std::memory_order_relaxed); }
  float rms() const { return m_rms.load(std::memory_order_relaxed); }
  bool clippedRecently(int holdMs = 1000) const;

signals:
  void streamStateChanged(QString state);

private:
  void connectStreamLocked();
  void destroyStreamLocked();

  static void onStreamStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error);
  static void onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param);
  static void onStreamProcess(void* data);

  void processBuffer(pw_buffer* buffer);

  PipeWireThread* m_pw = nullptr;
  pw_stream* m_stream = nullptr;
  spa_hook m_streamListener{};

  QString m_targetObject;

  std::atomic_bool m_captureSink{false};
  std::atomic<uint32_t> m_channels{2};
  std::atomic<float> m_peak{0.0f};
  std::atomic<float> m_rms{0.0f};
  std::atomic<int64_t> m_lastClipNs{0};
};
