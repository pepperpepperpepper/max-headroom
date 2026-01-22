#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

#include <pipewire/stream.h>
#include <sndfile.h>

class PipeWireThread;

class AudioRecorder final : public QObject
{
  Q_OBJECT

public:
  enum class Format {
    Wav,
    Flac,
  };

  struct StartOptions final {
    QString filePath;
    QString targetObject;
    bool captureSink = true;
    Format format = Format::Wav;
  };

  explicit AudioRecorder(PipeWireThread* pw, QObject* parent = nullptr);
  ~AudioRecorder() override;

  AudioRecorder(const AudioRecorder&) = delete;
  AudioRecorder& operator=(const AudioRecorder&) = delete;

  bool isRecording() const { return m_stream != nullptr; }

  QString filePath() const { return m_filePath; }
  QString targetObject() const { return m_targetObject; }
  bool captureSink() const { return m_captureSink.load(std::memory_order_relaxed); }
  Format format() const { return static_cast<Format>(m_format.load(std::memory_order_relaxed)); }

  uint32_t sampleRate() const { return m_sampleRate.load(std::memory_order_relaxed); }
  uint32_t channels() const { return m_channels.load(std::memory_order_relaxed); }
  uint32_t quantumFrames() const { return m_quantumFrames.load(std::memory_order_relaxed); }
  uint32_t lastBufferFrames() const { return m_lastBufferFrames.load(std::memory_order_relaxed); }
  uint32_t lastBufferBytes() const { return m_lastBufferBytes.load(std::memory_order_relaxed); }
  uint64_t dataBytesWritten() const { return m_dataBytesWritten.load(std::memory_order_relaxed); }
  uint64_t framesCaptured() const { return m_framesCaptured.load(std::memory_order_relaxed); }

  float peakDb() const { return m_peakDb.load(std::memory_order_relaxed); }
  float rmsDb() const { return m_rmsDb.load(std::memory_order_relaxed); }

  QString lastError() const;
  pw_stream_state streamState() const;
  QString streamStateString() const;

  static QString formatToString(Format format);
  static std::optional<Format> formatFromString(const QString& s);
  static QString defaultExtensionForFormat(Format format);
  static QString ensureFileExtension(QString path, Format format);

  static QString expandPathTemplate(const QString& templateOrPath, const QString& targetLabel, Format format);

  bool start(const StartOptions& options);
  bool startWav(const QString& filePath, const QString& targetObject, bool captureSink);
  void stop();

signals:
  void recordingChanged(bool recording);
  void errorOccurred(QString message);

private:
  static void onStreamStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error);
  static void onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param);
  static void onStreamProcess(void* data);

  void connectStreamLocked();
  void destroyStreamLocked();

  void processBuffer(pw_buffer* buffer);
  void setErrorAndScheduleStop(QString message);

  PipeWireThread* m_pw = nullptr;
  pw_stream* m_stream = nullptr;
  spa_hook m_streamListener{};

  SNDFILE* m_sndFile = nullptr;
  QString m_filePath;
  QString m_targetObject;
  std::atomic_bool m_captureSink{true};
  std::atomic_int m_format{static_cast<int>(Format::Wav)};

  std::atomic<uint32_t> m_sampleRate{48000};
  std::atomic<uint32_t> m_channels{2};
  std::atomic<uint32_t> m_quantumFrames{0};
  std::atomic<uint32_t> m_lastBufferFrames{0};
  std::atomic<uint32_t> m_lastBufferBytes{0};
  std::atomic<uint64_t> m_dataBytesWritten{0};
  std::atomic<uint64_t> m_framesCaptured{0};
  std::atomic<float> m_peakDb{-100.0f};
  std::atomic<float> m_rmsDb{-100.0f};
  std::atomic_bool m_stopScheduled{false};
  std::atomic_int m_streamState{PW_STREAM_STATE_UNCONNECTED};

  mutable std::mutex m_errorMutex;
  QString m_lastError;
};
