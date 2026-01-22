#include "LogStore.h"

#include <QDateTime>
#include <QMetaObject>
#include <QThread>

#include <pipewire/log.h>
#include <spa/support/log.h>
#include <spa/utils/hook.h>

namespace {

LogStore* g_logStore = nullptr;

QtMessageHandler g_prevQtHandler = nullptr;
bool g_forwardQt = true;

struct PipeWireLoggerState final {
  spa_log logger{};
  spa_log_methods methods{};
  spa_log* previous = nullptr;
  bool forward = true;
};

PipeWireLoggerState g_pw{};

QString levelTag(LogStore::Level level)
{
  switch (level) {
    case LogStore::Level::Error:
      return QStringLiteral("E");
    case LogStore::Level::Warning:
      return QStringLiteral("W");
    case LogStore::Level::Info:
      return QStringLiteral("I");
    case LogStore::Level::Debug:
      return QStringLiteral("D");
    case LogStore::Level::Trace:
      return QStringLiteral("T");
  }
  return QStringLiteral("?");
}

LogStore::Level fromSpaLevel(spa_log_level level)
{
  switch (level) {
    case SPA_LOG_LEVEL_ERROR:
      return LogStore::Level::Error;
    case SPA_LOG_LEVEL_WARN:
      return LogStore::Level::Warning;
    case SPA_LOG_LEVEL_INFO:
      return LogStore::Level::Info;
    case SPA_LOG_LEVEL_DEBUG:
      return LogStore::Level::Debug;
    case SPA_LOG_LEVEL_TRACE:
      return LogStore::Level::Trace;
    case SPA_LOG_LEVEL_NONE:
      break;
  }
  return LogStore::Level::Info;
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
  Q_UNUSED(context);

  LogStore* store = LogStore::instance();
  if (store) {
    LogStore::Level level = LogStore::Level::Info;
    switch (type) {
      case QtDebugMsg:
        level = LogStore::Level::Debug;
        break;
      case QtInfoMsg:
        level = LogStore::Level::Info;
        break;
      case QtWarningMsg:
        level = LogStore::Level::Warning;
        break;
      case QtCriticalMsg:
        level = LogStore::Level::Error;
        break;
      case QtFatalMsg:
        level = LogStore::Level::Error;
        break;
    }
    store->append(level, QStringLiteral("Qt"), message);
  }

  if (g_prevQtHandler && g_forwardQt) {
    g_prevQtHandler(type, context, message);
  }
}

void pwLogTopicInit(void* object, spa_log_topic* topic)
{
  Q_UNUSED(object);
  if (!topic) {
    return;
  }
  topic->has_custom_level = false;
  topic->level = g_pw.logger.level;
}

void pwLogLogtv(void* object,
                spa_log_level level,
                const spa_log_topic* topic,
                const char* file,
                int line,
                const char* func,
                const char* fmt,
                va_list args)
{
  Q_UNUSED(file);
  Q_UNUSED(line);
  Q_UNUSED(func);

  auto* store = static_cast<LogStore*>(object);
  if (store) {
    va_list argsCopy;
    va_copy(argsCopy, args);
    const QString msg = QString::vasprintf(fmt, argsCopy);
    va_end(argsCopy);

    QString source = QStringLiteral("PipeWire");
    if (topic && topic->topic) {
      source = QStringLiteral("PipeWire/%1").arg(QString::fromUtf8(topic->topic));
    }

    store->append(fromSpaLevel(level), source, msg);
  }

  if (g_pw.forward && g_pw.previous && g_pw.previous != &g_pw.logger) {
    va_list argsForward;
    va_copy(argsForward, args);
    spa_log_logtv(g_pw.previous, level, topic, file, line, func, fmt, argsForward);
    va_end(argsForward);
  }
}

} // namespace

LogStore::LogStore(QObject* parent)
    : QObject(parent)
{
  if (!g_logStore) {
    g_logStore = this;
  }
}

LogStore::~LogStore()
{
  if (g_logStore == this) {
    g_logStore = nullptr;
  }
}

LogStore* LogStore::instance()
{
  return g_logStore;
}

void LogStore::installQtMessageHandler(bool forwardToStderr)
{
  if (m_qtHandlerInstalled) {
    return;
  }

  g_forwardQt = forwardToStderr;
  g_prevQtHandler = qInstallMessageHandler(&qtMessageHandler);
  m_qtHandlerInstalled = true;
}

void LogStore::installPipeWireLogger(bool forwardToStderr)
{
  if (m_pipewireHandlerInstalled) {
    return;
  }

  g_pw.forward = forwardToStderr;
  g_pw.previous = pw_log_get();

  g_pw.methods = spa_log_methods{};
  g_pw.methods.version = SPA_VERSION_LOG_METHODS;
  g_pw.methods.logtv = &pwLogLogtv;
  g_pw.methods.topic_init = &pwLogTopicInit;

  g_pw.logger = spa_log{};
  g_pw.logger.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &g_pw.methods, this);
  g_pw.logger.level = g_pw.previous ? g_pw.previous->level : SPA_LOG_LEVEL_INFO;

  pw_log_set(&g_pw.logger);
  m_pipewireHandlerInstalled = true;
}

void LogStore::append(Level level, QString source, QString message)
{
  const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));
  const QString line = QStringLiteral("%1 [%2] %3: %4").arg(ts, levelTag(level), source, message);

  if (QThread::currentThread() == thread()) {
    appendLine(line);
    return;
  }

  QMetaObject::invokeMethod(this, [this, line]() { appendLine(line); }, Qt::QueuedConnection);
}

QStringList LogStore::lines() const
{
  return m_lines;
}

void LogStore::clear()
{
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this]() { clear(); }, Qt::QueuedConnection);
    return;
  }
  m_lines.clear();
  emit cleared();
}

void LogStore::appendLine(QString line)
{
  m_lines.push_back(std::move(line));
  while (m_lines.size() > m_maxLines) {
    m_lines.pop_front();
  }
  emit lineAdded(m_lines.back());
}
