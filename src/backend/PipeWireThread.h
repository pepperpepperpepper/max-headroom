#pragma once

#include <QObject>

#include <atomic>

#include <pipewire/core.h>
#include <pipewire/thread-loop.h>

class PipeWireThread final : public QObject
{
  Q_OBJECT

public:
  explicit PipeWireThread(QObject* parent = nullptr);
  ~PipeWireThread() override;

  PipeWireThread(const PipeWireThread&) = delete;
  PipeWireThread& operator=(const PipeWireThread&) = delete;

  pw_thread_loop* threadLoop() const { return m_loop; }
  pw_core* core() const { return m_core; }

  bool isConnected() const { return m_connected.load(); }

signals:
  void connectionChanged(bool connected);
  void errorOccurred(QString message);

private:
  static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message);
  static void onCoreDone(void* data, uint32_t id, int seq);

  pw_thread_loop* m_loop = nullptr;
  pw_context* m_context = nullptr;
  pw_core* m_core = nullptr;

  spa_hook m_coreListener{};

  std::atomic_bool m_connected{false};
  int m_initialSyncSeq = -1;
  bool m_initialSyncDone = false;
  bool m_initialSyncFailed = false;
  QString m_initialSyncError;
};
