#include "AlsaSeqBridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString configBaseDir()
{
  QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  if (!base.trimmed().isEmpty()) {
    return base;
  }

  const QString home = QDir::homePath();
  if (!home.trimmed().isEmpty()) {
    return QDir(home).filePath(QStringLiteral(".config"));
  }

  return QDir::tempPath();
}

QByteArray alsaSeqSnippetContents()
{
  return QByteArrayLiteral(
      "# Managed by Headroom.\n"
      "# Enables ALSA Sequencer MIDI bridging (ALSA seq <-> PipeWire MIDI).\n"
      "\n"
      "context.modules = [\n"
      "    { name = libpipewire-module-alsa-seq\n"
      "      flags = [ ifexists nofail ]\n"
      "    }\n"
      "]\n");
}

} // namespace

QString AlsaSeqBridge::configSnippetPath()
{
  QDir d(configBaseDir());
  return d.filePath(QStringLiteral("pipewire/pipewire.conf.d/headroom-alsa-seq.conf"));
}

bool AlsaSeqBridge::isConfigInstalled()
{
  return QFileInfo::exists(configSnippetPath());
}

bool AlsaSeqBridge::installConfig(QString* errorOut)
{
  const QString path = configSnippetPath();

  QDir dir(QFileInfo(path).absolutePath());
  if (!dir.mkpath(QStringLiteral("."))) {
    if (errorOut) {
      *errorOut = QStringLiteral("failed to create directory: %1").arg(dir.absolutePath());
    }
    return false;
  }

  const QByteArray desired = alsaSeqSnippetContents();

  QFile existing(path);
  if (existing.open(QIODevice::ReadOnly)) {
    if (existing.readAll() == desired) {
      return true;
    }
  }

  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (errorOut) {
      *errorOut = QStringLiteral("open failed: %1").arg(f.errorString());
    }
    return false;
  }

  if (f.write(desired) != desired.size()) {
    if (errorOut) {
      *errorOut = QStringLiteral("write failed: %1").arg(f.errorString());
    }
    return false;
  }

  if (!f.commit()) {
    if (errorOut) {
      *errorOut = QStringLiteral("commit failed: %1").arg(f.errorString());
    }
    return false;
  }

  return true;
}

bool AlsaSeqBridge::removeConfig(QString* errorOut)
{
  const QString path = configSnippetPath();
  if (!QFileInfo::exists(path)) {
    return true;
  }

  if (!QFile::remove(path)) {
    if (errorOut) {
      *errorOut = QStringLiteral("failed to remove: %1").arg(path);
    }
    return false;
  }
  return true;
}

bool AlsaSeqBridge::alsaSequencerDevicePresent()
{
  return QFileInfo::exists(QStringLiteral("/dev/snd/seq"));
}

