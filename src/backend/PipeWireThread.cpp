#include "PipeWireThread.h"

#include <QCoreApplication>
#include <QString>

#include <pipewire/context.h>
#include <pipewire/impl-module.h>

#include <spa/utils/result.h>

#include <errno.h>

PipeWireThread::PipeWireThread(QObject* parent)
    : QObject(parent)
{
  m_loop = pw_thread_loop_new("headroom-pw", nullptr);
  if (!m_loop) {
    emit errorOccurred(tr("Failed to create PipeWire thread loop"));
    return;
  }

  pw_thread_loop_start(m_loop);

  pw_thread_loop_lock(m_loop);

  m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
  if (!m_context) {
    pw_thread_loop_unlock(m_loop);
    emit errorOccurred(tr("Failed to create PipeWire context"));
    return;
  }

  // Client-side support for PipeWire extension interfaces can be modular.
  // In particular, the Profiler interface requires a protocol marshal that
  // is not always enabled by default. Loading this module is safe even when
  // the remote does not expose a Profiler object; it simply registers client
  // types/marshals when available.
  pw_context_load_module(m_context, "libpipewire-module-profiler", nullptr, nullptr);

  m_core = pw_context_connect(m_context, nullptr, 0);
  if (!m_core) {
    pw_thread_loop_unlock(m_loop);
    emit errorOccurred(tr("Failed to connect to PipeWire"));
    return;
  }

  static const pw_core_events coreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = &PipeWireThread::onCoreDone;
    e.error = &PipeWireThread::onCoreError;
    return e;
  }();
  pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);

  m_initialSyncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
  m_initialSyncDone = false;
  m_initialSyncFailed = false;
  m_initialSyncError.clear();
  const int waitRes = pw_thread_loop_timed_wait(m_loop, 1);
  if (waitRes < 0 || !m_initialSyncDone || m_initialSyncFailed) {
    const QString msg = m_initialSyncError.isEmpty() ? tr("Failed to connect to PipeWire") : m_initialSyncError;

    spa_hook_remove(&m_coreListener);
    pw_core_disconnect(m_core);
    m_core = nullptr;
    pw_context_destroy(m_context);
    m_context = nullptr;

    pw_thread_loop_unlock(m_loop);
    emit errorOccurred(msg);
    return;
  }

  pw_thread_loop_unlock(m_loop);

  m_connected.store(true);
  emit connectionChanged(true);
}

PipeWireThread::~PipeWireThread()
{
  if (!m_loop) {
    return;
  }

  pw_thread_loop_lock(m_loop);

  if (m_core) {
    spa_hook_remove(&m_coreListener);
    pw_core_disconnect(m_core);
    m_core = nullptr;
  }

  if (m_context) {
    pw_context_destroy(m_context);
    m_context = nullptr;
  }

  pw_thread_loop_unlock(m_loop);

  pw_thread_loop_stop(m_loop);
  pw_thread_loop_destroy(m_loop);
  m_loop = nullptr;
}

void PipeWireThread::onCoreError(void* data, uint32_t id, int seq, int res, const char* message)
{
  Q_UNUSED(id);

  auto* self = static_cast<PipeWireThread*>(data);
  const QString msg = QStringLiteral("%1 (%2)").arg(QString::fromUtf8(message), QString::fromUtf8(spa_strerror(res)));

  if (seq == self->m_initialSyncSeq) {
    self->m_initialSyncFailed = true;
    self->m_initialSyncError = msg;
    pw_thread_loop_signal(self->m_loop, false);
  }

  QMetaObject::invokeMethod(self, [self, msg]() { emit self->errorOccurred(msg); }, Qt::QueuedConnection);

  if (res == -EPIPE || res == -ECONNRESET || res == -EHOSTDOWN || res == -ENOTCONN || res == -ECONNREFUSED) {
    self->m_connected.store(false);
    QMetaObject::invokeMethod(self, [self]() { emit self->connectionChanged(false); }, Qt::QueuedConnection);
  }
}

void PipeWireThread::onCoreDone(void* data, uint32_t /*id*/, int seq)
{
  auto* self = static_cast<PipeWireThread*>(data);
  if (seq == self->m_initialSyncSeq) {
    self->m_initialSyncDone = true;
    pw_thread_loop_signal(self->m_loop, false);
  }
}
