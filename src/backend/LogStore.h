#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class LogStore final : public QObject
{
  Q_OBJECT

public:
  enum class Level {
    Error,
    Warning,
    Info,
    Debug,
    Trace,
  };

  explicit LogStore(QObject* parent = nullptr);
  ~LogStore() override;

  LogStore(const LogStore&) = delete;
  LogStore& operator=(const LogStore&) = delete;

  static LogStore* instance();

  void installQtMessageHandler(bool forwardToStderr = true);
  void installPipeWireLogger(bool forwardToStderr = true);

  void append(Level level, QString source, QString message);
  QStringList lines() const;
  void clear();

signals:
  void lineAdded(QString line);
  void cleared();

private:
  void appendLine(QString line);

  QStringList m_lines;
  int m_maxLines = 2000;

  bool m_qtHandlerInstalled = false;
  bool m_pipewireHandlerInstalled = false;
};

