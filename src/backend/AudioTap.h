#pragma once

#include <QObject>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include <pipewire/stream.h>

class PipeWireThread;
struct VisualizerSettings;

struct SpectrogramSnapshot final {
  int columns = 0;
  int bins = 0;
  int writeColumn = 0;
  QVector<uint32_t> pixels; // columns*bins, column-major
};

class AudioTap final : public QObject
{
  Q_OBJECT

public:
  explicit AudioTap(PipeWireThread* pw, QObject* parent = nullptr);
  ~AudioTap() override;

  AudioTap(const AudioTap&) = delete;
  AudioTap& operator=(const AudioTap&) = delete;

  QString targetObject() const { return m_targetObject; }
  void setTargetObject(const QString& targetObject);

  bool captureSink() const { return m_captureSink.load(std::memory_order_relaxed); }
  void setCaptureSink(bool captureSink);

  // Convenience method to update captureSink + targetObject with a single reconnect.
  void setTarget(bool captureSink, const QString& targetObject);

  bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
  void setEnabled(bool enabled);

  uint32_t sampleRate() const;
  void applySettings(const VisualizerSettings& settings);

  float peak() const { return m_peak.load(std::memory_order_relaxed); }
  float rms() const { return m_rms.load(std::memory_order_relaxed); }

  QVector<float> spectrum() const;
  SpectrogramSnapshot spectrogram() const;

  // Returns per-column min/max pairs for drawing a waveform efficiently.
  // windowSamples controls how much history to include.
  void waveformMinMax(int columns, int windowSamples, QVector<float>& minsOut, QVector<float>& maxsOut) const;

signals:
  void streamStateChanged(QString state);

private:
  void connectStreamLocked();
  void destroyStreamLocked();

  void resizeWaveRingLocked();
  void resizeSpectrogramLocked();

  static void onStreamStateChanged(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error);
  static void onStreamParamChanged(void* data, uint32_t id, const struct spa_pod* param);
  static void onStreamProcess(void* data);

  void processBuffer(pw_buffer* buffer);

  static uint32_t spectrogramColor(float normalized01);

  PipeWireThread* m_pw = nullptr;
  pw_stream* m_stream = nullptr;
  spa_hook m_streamListener{};

  std::atomic_bool m_enabled{true};
  std::atomic_bool m_captureSink{false};
  QString m_targetObject;

  // Format (updated when negotiated)
  uint32_t m_sampleRate = 48000;
  uint32_t m_channels = 2;

  double m_waveHistorySeconds = 0.5;
  double m_spectrogramHistorySeconds = 2.0;
  float m_spectrumSmoothing = 0.92f;

  // Wave ring
  std::vector<float> m_waveRing;
  std::size_t m_waveWrite = 0;
  std::size_t m_waveCount = 0;

  // FFT/spectrogram
  std::size_t m_fftSize = 1024;
  std::size_t m_hopSize = 256;
  std::vector<float> m_hann;
  std::vector<float> m_fftRing;
  std::size_t m_fftWrite = 0;
  std::size_t m_fftTotal = 0;
  std::size_t m_sinceFrame = 0;

  QVector<float> m_spectrum; // bins
  int m_specColumns = 360;
  int m_specBins = 512;
  int m_specWriteCol = 0;
  QVector<uint32_t> m_specPixels; // column-major

  mutable std::mutex m_mutex;

  std::atomic<float> m_peak{0.0f};
  std::atomic<float> m_rms{0.0f};
};
