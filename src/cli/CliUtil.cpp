#include "cli/CliInternal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <errno.h>
#include <signal.h>
#include <unistd.h>

namespace headroomctl {
void printUsage(QTextStream& out)
{
  out << "headroomctl\n"
         "\n"
         "Usage:\n"
         "  headroomctl --version\n"
         "  headroomctl nodes\n"
         "  headroomctl sinks\n"
         "  headroomctl sinks order [get]\n"
         "  headroomctl sinks order move <node-id|node-name> up|down|top|bottom\n"
         "  headroomctl sinks order reset\n"
         "  headroomctl sources\n"
         "  headroomctl default-sink [get]\n"
         "  headroomctl default-sink set <node-id|node-name>\n"
         "  headroomctl default-source [get]\n"
         "  headroomctl default-source set <node-id|node-name>\n"
         "  headroomctl ports\n"
         "  headroomctl links\n"
         "  headroomctl connect <out-port-id> <in-port-id> [--force]\n"
         "  headroomctl disconnect <link-id> [--force]\n"
         "  headroomctl disconnect <out-port-id> <in-port-id> [--force]\n"
         "  headroomctl patchbay profiles\n"
         "  headroomctl patchbay save <profile-name>\n"
         "  headroomctl patchbay apply <profile-name> [--strict] [--no-hooks]\n"
         "  headroomctl patchbay delete <profile-name>\n"
         "  headroomctl patchbay hooks get <profile-name>\n"
         "  headroomctl patchbay hooks set <profile-name> load|unload <command...>\n"
         "  headroomctl patchbay hooks clear <profile-name> load|unload|all\n"
         "  headroomctl patchbay port status <port-id>\n"
         "  headroomctl patchbay port alias get <port-id>\n"
         "  headroomctl patchbay port alias set <port-id> <alias>\n"
         "  headroomctl patchbay port alias clear <port-id>\n"
         "  headroomctl patchbay port lock get <port-id>\n"
         "  headroomctl patchbay port lock set <port-id> on|off|toggle\n"
         "  headroomctl patchbay autoconnect status\n"
         "  headroomctl patchbay autoconnect enable on|off|toggle\n"
         "  headroomctl patchbay autoconnect apply\n"
         "  headroomctl patchbay autoconnect rules\n"
         "  headroomctl patchbay autoconnect rule add <name> <out-node-re> <out-port-re> <in-node-re> <in-port-re> [--disabled]\n"
         "  headroomctl patchbay autoconnect rule enable <name> on|off|toggle\n"
         "  headroomctl patchbay autoconnect rule delete <name>\n"
         "  headroomctl patchbay autoconnect whitelist list|add|remove <regex>\n"
         "  headroomctl patchbay autoconnect blacklist list|add|remove <regex>\n"
         "  headroomctl session list\n"
         "  headroomctl session save <snapshot-name>\n"
         "  headroomctl session apply <snapshot-name> [--strict-links] [--merge-settings]\n"
         "  headroomctl session delete <snapshot-name>\n"
         "  headroomctl eq list\n"
         "  headroomctl eq get <node-id|node-name>\n"
         "  headroomctl eq enable <node-id|node-name> on|off|toggle\n"
         "  headroomctl eq preset <node-id|node-name> <preset-name>\n"
         "  headroomctl eq presets\n"
         "  headroomctl record targets\n"
         "  headroomctl record start <file|template> [--format wav|flac] [--duration <sec>] [--target <node-id|node-name>] [--sink|--source] [--background]\n"
         "  headroomctl record stop\n"
         "  headroomctl record status\n"
         "  headroomctl engine status\n"
         "  headroomctl engine start <pipewire|wireplumber|pipewire-pulse|unit|all>\n"
         "  headroomctl engine stop <pipewire|wireplumber|pipewire-pulse|unit|all>\n"
         "  headroomctl engine restart <pipewire|wireplumber|pipewire-pulse|unit|all>\n"
         "  headroomctl engine midi-bridge [status]\n"
         "  headroomctl engine midi-bridge enable on|off|toggle\n"
         "  headroomctl engine clock status\n"
         "  headroomctl engine clock presets\n"
         "  headroomctl engine clock preset <preset-id>\n"
         "  headroomctl engine clock set [--rate <hz|auto>] [--quantum <frames|auto>] [--min-quantum <frames|auto>] [--max-quantum <frames|auto>]\n"
         "  headroomctl diagnostics [status|drivers]\n"
         "  headroomctl set-volume <node-id> <value>\n"
         "  headroomctl mute <node-id> on|off|toggle\n"
         "\n"
         "Options:\n"
         "  --json  Output machine-readable JSON (where applicable)\n"
         "\n"
         "Notes:\n"
         "  <value> accepts: 0-200 (percent), or 0.0-2.0 (linear), or with a % suffix.\n"
         "  Recording templates: {datetime} {date} {time} {target} {ext} {format}\n";
}

void waitForGraph(int waitMs)
{
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < waitMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(10);
  }
}

std::optional<uint32_t> parseNodeId(const QString& s)
{
  bool ok = false;
  const uint32_t id = s.toUInt(&ok);
  if (!ok) {
    return std::nullopt;
  }
  return id;
}

std::optional<float> parseVolumeValue(const QString& input)
{
  QString s = input.trimmed();
  bool isPercent = false;
  if (s.endsWith('%')) {
    s.chop(1);
    isPercent = true;
  }

  bool ok = false;
  const double v = s.toDouble(&ok);
  if (!ok) {
    return std::nullopt;
  }

  if (isPercent || v > 2.0) {
    return static_cast<float>(v / 100.0);
  }
  return static_cast<float>(v);
}

QString nodeLabel(const PwNodeInfo& n)
{
  return n.description.isEmpty() ? n.name : n.description;
}

QString runtimeDirPath()
{
  QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (dir.trimmed().isEmpty()) {
    dir = QDir::tempPath();
  }
  return dir;
}

QString recordingStatusPath()
{
  const QString base = runtimeDirPath();
  QDir d(base);
  d.mkpath(QStringLiteral("headroom"));
  return d.filePath(QStringLiteral("headroom/recording.json"));
}

bool writeJsonFileAtomic(const QString& path, const QJsonObject& obj, QString* errorOut)
{
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (errorOut) {
      *errorOut = QStringLiteral("open failed: %1").arg(f.errorString());
    }
    return false;
  }

  const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
  if (f.write(data) != data.size()) {
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

std::optional<QJsonObject> readJsonObjectFile(const QString& path, QString* errorOut)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (errorOut) {
      *errorOut = QStringLiteral("open failed: %1").arg(f.errorString());
    }
    return std::nullopt;
  }
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    if (errorOut) {
      *errorOut = QStringLiteral("parse failed");
    }
    return std::nullopt;
  }
  return doc.object();
}

bool pidAlive(qint64 pid)
{
  if (pid <= 0) {
    return false;
  }
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

std::optional<bool> processRunningExact(const QString& name)
{
  if (name.trimmed().isEmpty()) {
    return std::nullopt;
  }
  if (QStandardPaths::findExecutable(QStringLiteral("pgrep")).isEmpty()) {
    return std::nullopt;
  }

  QProcess p;
  p.setProgram(QStringLiteral("pgrep"));
  p.setArguments({QStringLiteral("-x"), name});
  p.setProcessChannelMode(QProcess::SeparateChannels);
  p.start();

  if (!p.waitForStarted(500)) {
    return std::nullopt;
  }
  if (!p.waitForFinished(1500)) {
    p.kill();
    p.waitForFinished(300);
    return std::nullopt;
  }

  if (p.exitStatus() != QProcess::NormalExit) {
    return std::nullopt;
  }

  if (p.exitCode() == 0) {
    return true;
  }
  if (p.exitCode() == 1) {
    return false;
  }
  return std::nullopt;
}

bool stopPid(qint64 pid, int timeoutMs, QTextStream& err)
{
  if (pid <= 0) {
    err << "headroomctl: invalid pid\n";
    return false;
  }
  if (!pidAlive(pid)) {
    return true;
  }

  const pid_t p = static_cast<pid_t>(pid);
  ::kill(p, SIGINT);

  QElapsedTimer t;
  t.start();
  while (t.elapsed() < timeoutMs) {
    if (!pidAlive(pid)) {
      return true;
    }
    QThread::msleep(50);
  }

  ::kill(p, SIGTERM);
  t.restart();
  while (t.elapsed() < 800) {
    if (!pidAlive(pid)) {
      return true;
    }
    QThread::msleep(50);
  }

  ::kill(p, SIGKILL);
  t.restart();
  while (t.elapsed() < 800) {
    if (!pidAlive(pid)) {
      return true;
    }
    QThread::msleep(50);
  }

  err << "headroomctl: failed to stop recording process " << pid << "\n";
  return false;
}

QString eqPresetKeyForNodeName(const QString& nodeName)
{
  return QStringLiteral("eq/%1/presetJson").arg(nodeName);
}

EqPreset loadEqPresetForNodeName(const QString& nodeName)
{
  QSettings s;
  const QString json = s.value(eqPresetKeyForNodeName(nodeName)).toString();
  if (json.trimmed().isEmpty()) {
    return defaultEqPreset(6);
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return defaultEqPreset(6);
  }

  return eqPresetFromJson(doc.object());
}

void saveEqPresetForNodeName(const QString& nodeName, const EqPreset& preset)
{
  QSettings s;
  const QJsonDocument doc(eqPresetToJson(preset));
  s.setValue(eqPresetKeyForNodeName(nodeName), QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

QString normalizePresetName(const QString& in)
{
  QString out;
  out.reserve(in.size());
  const QString s = in.trimmed().toLower();
  for (const QChar ch : s) {
    if (ch.isLetterOrNumber()) {
      out.append(ch);
    }
  }
  return out;
}

QVector<QPair<QString, EqPreset>> builtinEqPresets()
{
  QVector<QPair<QString, EqPreset>> out;

  EqPreset flat = defaultEqPreset(6);
  flat.enabled = true;

  EqPreset bass = flat;
  if (!bass.bands.isEmpty()) {
    bass.bands[0].type = EqBandType::LowShelf;
    bass.bands[0].freqHz = 90.0;
    bass.bands[0].q = 0.707;
    bass.bands[0].gainDb = 4.0;
  }
  if (bass.bands.size() > 1) {
    bass.bands[1].gainDb = 2.0;
  }

  EqPreset treble = flat;
  if (treble.bands.size() > 4) {
    treble.bands[4].gainDb = 2.5;
  }
  if (treble.bands.size() > 5) {
    treble.bands[5].type = EqBandType::HighShelf;
    treble.bands[5].freqHz = 8000.0;
    treble.bands[5].q = 0.707;
    treble.bands[5].gainDb = 4.0;
  }

  EqPreset vocal = flat;
  if (vocal.bands.size() > 2) {
    vocal.bands[2].gainDb = -1.0;
  }
  if (vocal.bands.size() > 3) {
    vocal.bands[3].gainDb = 3.0;
    vocal.bands[3].q = 1.2;
  }

  EqPreset loudness = flat;
  if (!loudness.bands.isEmpty()) {
    loudness.bands[0].type = EqBandType::LowShelf;
    loudness.bands[0].freqHz = 90.0;
    loudness.bands[0].q = 0.707;
    loudness.bands[0].gainDb = 3.5;
  }
  if (loudness.bands.size() > 3) {
    loudness.bands[3].gainDb = -2.0;
  }
  if (loudness.bands.size() > 5) {
    loudness.bands[5].type = EqBandType::HighShelf;
    loudness.bands[5].freqHz = 9000.0;
    loudness.bands[5].q = 0.707;
    loudness.bands[5].gainDb = 3.0;
  }

  out.push_back(qMakePair(QStringLiteral("Flat"), flat));
  out.push_back(qMakePair(QStringLiteral("Bass Boost"), bass));
  out.push_back(qMakePair(QStringLiteral("Treble Boost"), treble));
  out.push_back(qMakePair(QStringLiteral("Vocal"), vocal));
  out.push_back(qMakePair(QStringLiteral("Loudness (V)"), loudness));
  return out;
}

std::optional<EqPreset> builtinEqPresetByName(const QString& presetName)
{
  const QString k = normalizePresetName(presetName);
  const auto presets = builtinEqPresets();
  for (const auto& p : presets) {
    if (normalizePresetName(p.first) == k) {
      return p.second;
    }
  }
  if (k == QStringLiteral("bass")) {
    return builtinEqPresetByName(QStringLiteral("Bass Boost"));
  }
  if (k == QStringLiteral("treble")) {
    return builtinEqPresetByName(QStringLiteral("Treble Boost"));
  }
  if (k == QStringLiteral("loudness")) {
    return builtinEqPresetByName(QStringLiteral("Loudness (V)"));
  }
  return std::nullopt;
}

QString resolveNodeName(const QString& nodeIdOrName, PipeWireGraph* graph)
{
  bool ok = false;
  const uint32_t maybeId = nodeIdOrName.trimmed().toUInt(&ok);
  if (ok && graph) {
    if (const auto n = graph->nodeById(maybeId)) {
      if (!n->name.isEmpty()) {
        return n->name;
      }
    }
  }
  return nodeIdOrName.trimmed();
}

void printNodes(QTextStream& out, const QList<PwNodeInfo>& nodes, PipeWireGraph& graph)
{
  for (const auto& n : nodes) {
    const PwNodeControls c = graph.nodeControls(n.id).value_or(PwNodeControls{});
    const int volPct = static_cast<int>(qRound(c.volume * 100.0f));
    out << n.id << "\t" << nodeLabel(n) << "\t" << volPct << "%\t" << (c.mute ? "muted" : "unmuted") << "\n";
  }
}

void printAllNodes(QTextStream& out, const QList<PwNodeInfo>& nodes)
{
  for (const auto& n : nodes) {
    out << n.id << "\t" << n.mediaClass << "\t" << nodeLabel(n) << "\n";
  }
}

std::optional<PwPortInfo> portById(const QList<PwPortInfo>& ports, uint32_t portId)
{
  for (const auto& p : ports) {
    if (p.id == portId) {
      return p;
    }
  }
  return std::nullopt;
}

std::optional<PwLinkInfo> linkByPorts(const QList<PwLinkInfo>& links, uint32_t outPortId, uint32_t inPortId)
{
  for (const auto& l : links) {
    if (l.outputPortId == outPortId && l.inputPortId == inPortId) {
      return l;
    }
  }
  return std::nullopt;
}

QJsonObject nodeToJson(const PwNodeInfo& n)
{
  QJsonObject o;
  o.insert(QStringLiteral("id"), static_cast<qint64>(n.id));
  o.insert(QStringLiteral("name"), n.name);
  o.insert(QStringLiteral("description"), nodeLabel(n));
  o.insert(QStringLiteral("mediaClass"), n.mediaClass);
  o.insert(QStringLiteral("appName"), n.appName);
  o.insert(QStringLiteral("appProcessBinary"), n.appProcessBinary);
  o.insert(QStringLiteral("objectSerial"), n.objectSerial);
  return o;
}

QJsonObject nodeControlsToJson(const PwNodeControls& c)
{
  QJsonObject o;
  o.insert(QStringLiteral("hasVolume"), c.hasVolume);
  o.insert(QStringLiteral("hasMute"), c.hasMute);
  o.insert(QStringLiteral("volume"), c.volume);
  o.insert(QStringLiteral("mute"), c.mute);

  QJsonArray channelVolumes;
  for (float f : c.channelVolumes) {
    channelVolumes.append(f);
  }
  o.insert(QStringLiteral("channelVolumes"), channelVolumes);
  return o;
}

QJsonObject portToJson(const PwPortInfo& p, const std::optional<PwNodeInfo>& node)
{
  QJsonObject o;
  o.insert(QStringLiteral("id"), static_cast<qint64>(p.id));
  o.insert(QStringLiteral("nodeId"), static_cast<qint64>(p.nodeId));
  o.insert(QStringLiteral("name"), p.name);
  o.insert(QStringLiteral("alias"), p.alias);
  o.insert(QStringLiteral("direction"), p.direction);
  o.insert(QStringLiteral("audioChannel"), p.audioChannel);
  o.insert(QStringLiteral("mediaType"), p.mediaType);
  o.insert(QStringLiteral("formatDsp"), p.formatDsp);
  if (node) {
    o.insert(QStringLiteral("nodeName"), node->name);
    o.insert(QStringLiteral("nodeDescription"), nodeLabel(*node));
    o.insert(QStringLiteral("nodeMediaClass"), node->mediaClass);
  }
  return o;
}

QJsonObject linkToJson(const PwLinkInfo& l,
                       const std::optional<PwNodeInfo>& outNode,
                       const std::optional<PwPortInfo>& outPort,
                       const std::optional<PwNodeInfo>& inNode,
                       const std::optional<PwPortInfo>& inPort)
{
  QJsonObject o;
  o.insert(QStringLiteral("id"), static_cast<qint64>(l.id));
  o.insert(QStringLiteral("outputNodeId"), static_cast<qint64>(l.outputNodeId));
  o.insert(QStringLiteral("outputPortId"), static_cast<qint64>(l.outputPortId));
  o.insert(QStringLiteral("inputNodeId"), static_cast<qint64>(l.inputNodeId));
  o.insert(QStringLiteral("inputPortId"), static_cast<qint64>(l.inputPortId));

  if (outNode) {
    o.insert(QStringLiteral("outputNodeName"), outNode->name);
    o.insert(QStringLiteral("outputNodeDescription"), nodeLabel(*outNode));
    o.insert(QStringLiteral("outputNodeMediaClass"), outNode->mediaClass);
  }
  if (outPort) {
    o.insert(QStringLiteral("outputPortName"), outPort->name);
    o.insert(QStringLiteral("outputPortAlias"), outPort->alias);
    o.insert(QStringLiteral("outputPortAudioChannel"), outPort->audioChannel);
    o.insert(QStringLiteral("outputPortMediaType"), outPort->mediaType);
  }
  if (inNode) {
    o.insert(QStringLiteral("inputNodeName"), inNode->name);
    o.insert(QStringLiteral("inputNodeDescription"), nodeLabel(*inNode));
    o.insert(QStringLiteral("inputNodeMediaClass"), inNode->mediaClass);
  }
  if (inPort) {
    o.insert(QStringLiteral("inputPortName"), inPort->name);
    o.insert(QStringLiteral("inputPortAlias"), inPort->alias);
    o.insert(QStringLiteral("inputPortAudioChannel"), inPort->audioChannel);
    o.insert(QStringLiteral("inputPortMediaType"), inPort->mediaType);
  }
  return o;
}
} // namespace headroomctl

