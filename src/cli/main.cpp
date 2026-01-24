#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPair>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <limits>
#include <optional>

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "backend/AudioRecorder.h"
#include "backend/AlsaSeqBridge.h"
#include "backend/EngineControl.h"
#include "backend/PatchbayProfiles.h"
#include "backend/PatchbayProfileHooks.h"
#include "backend/PatchbayPortConfig.h"
#include "backend/PatchbayAutoConnectRules.h"
#include "backend/SessionSnapshots.h"
#include "backend/PipeWireGraph.h"
#include "backend/PipeWireThread.h"
#include "backend/EqConfig.h"
#include "settings/SettingsKeys.h"

namespace {

std::atomic_bool g_recordStopRequested{false};

extern "C" void onRecordSignal(int)
{
  g_recordStopRequested.store(true);
}

void printUsage(QTextStream& out)
{
  out << "headroomctl\n"
         "\n"
         "Usage:\n"
         "  headroomctl --version\n"
         "  headroomctl nodes\n"
         "  headroomctl sinks\n"
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

} // namespace

int main(int argc, char** argv)
{
  pw_init(&argc, &argv);

  int exitCode = 0;
  {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Headroom"));
    QCoreApplication::setOrganizationName(QStringLiteral("maxheadroom"));
    QCoreApplication::setApplicationVersion(QStringLiteral(HEADROOM_VERSION));

    QTextStream out(stdout);
    QTextStream err(stderr);

    QStringList args = app.arguments();

    do {
      if (args.contains(QStringLiteral("--version")) || args.contains(QStringLiteral("-V"))) {
        out << QCoreApplication::applicationVersion() << "\n";
        exitCode = 0;
        break;
      }
      if (args.size() < 2) {
        printUsage(out);
        exitCode = 2;
        break;
      }

      const bool jsonOutput = args.contains(QStringLiteral("--json"));
      if (jsonOutput) {
        args.removeAll(QStringLiteral("--json"));
      }
      if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h")) ||
          args.contains(QStringLiteral("help"))) {
        printUsage(out);
        exitCode = 0;
        break;
      }

      const QString cmd = args.at(1).trimmed().toLower();
      if (cmd == QStringLiteral("version")) {
        out << QCoreApplication::applicationVersion() << "\n";
        exitCode = 0;
        break;
      }
      if (cmd == QStringLiteral("record")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        const QString statusPath = recordingStatusPath();

        if (sub == QStringLiteral("status")) {
          QString readErr;
          const auto obj = readJsonObjectFile(statusPath, &readErr);
          if (!obj) {
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("recording"), false);
              o.insert(QStringLiteral("statusPath"), statusPath);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "not-recording\n";
            }
            exitCode = 0;
            break;
          }

          const qint64 pid = obj->value(QStringLiteral("pid")).toVariant().toLongLong();
          const bool running = pidAlive(pid) && obj->value(QStringLiteral("running")).toBool(true);

          if (jsonOutput) {
            QJsonObject o = *obj;
            o.insert(QStringLiteral("recording"), running);
            o.insert(QStringLiteral("statusPath"), statusPath);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            const QString fmt = obj->value(QStringLiteral("format")).toString(QStringLiteral("wav"));
            const double dur = obj->value(QStringLiteral("durationSeconds")).toDouble(0.0);
            const double peak = obj->value(QStringLiteral("peakDb")).toDouble(std::numeric_limits<double>::quiet_NaN());
            const double rms = obj->value(QStringLiteral("rmsDb")).toDouble(std::numeric_limits<double>::quiet_NaN());
            out << (running ? "recording" : "stopped") << "\tpid=" << pid << "\tfile=" << obj->value(QStringLiteral("filePath")).toString()
                << "\tfmt=" << fmt << "\tdur=" << QString::number(dur, 'f', 1) << "s"
                << "\tpeak=" << (std::isfinite(peak) ? QString::number(peak, 'f', 1) + "dB" : QStringLiteral("-"))
                << "\trms=" << (std::isfinite(rms) ? QString::number(rms, 'f', 1) + "dB" : QStringLiteral("-"))
                << "\tbytes=" << obj->value(QStringLiteral("bytesWritten")).toVariant().toLongLong()
                << "\tstate=" << obj->value(QStringLiteral("streamState")).toString() << "\n";
          }

          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("stop")) {
          QString readErr;
          const auto obj = readJsonObjectFile(statusPath, &readErr);
          if (!obj) {
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("alreadyStopped"), true);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "not-recording\n";
            }
            exitCode = 0;
            break;
          }

          const qint64 pid = obj->value(QStringLiteral("pid")).toVariant().toLongLong();
          const bool ok = stopPid(pid, 2000, err);
          if (!ok) {
            exitCode = 1;
            break;
          }

          if (!pidAlive(pid)) {
            QFile::remove(statusPath);
          }

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("pid"), pid);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "stopped\tpid=" << pid << "\n";
          }
          exitCode = 0;
          break;
        }

        // record start/run handled below (requires PipeWire).
      }

      if (cmd == QStringLiteral("engine")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        const bool systemctlOk = EngineControl::isSystemctlAvailable();
        QString systemdErr;
        const bool userSystemdOk = systemctlOk ? EngineControl::canTalkToUserSystemd(&systemdErr) : false;

	        auto statusToJson = [](const SystemdUnitStatus& st) {
	          QJsonObject o;
	          o.insert(QStringLiteral("unit"), st.unit);
	          o.insert(QStringLiteral("loadState"), st.loadState);
	          o.insert(QStringLiteral("activeState"), st.activeState);
	          o.insert(QStringLiteral("subState"), st.subState);
	          o.insert(QStringLiteral("description"), st.description);
	          o.insert(QStringLiteral("exists"), st.exists());
	          o.insert(QStringLiteral("active"), st.isActive());
	          if (!st.error.isEmpty()) {
	            o.insert(QStringLiteral("error"), st.error);
	          }
	          return o;
	        };

	        if (sub == QStringLiteral("midi-bridge")) {
	          const QString bridgeSub = (args.size() >= 4) ? args.at(3).trimmed().toLower() : QStringLiteral("status");

	          auto moduleLoadedOpt = [&]() -> std::optional<bool> {
	            PipeWireThread pw;
	            PipeWireGraph graph(&pw);
	            if (!pw.isConnected()) {
	              return std::nullopt;
	            }
	            waitForGraph(200);
	            bool loaded = false;
	            for (const auto& m : graph.modules()) {
	              if (m.name.toLower().contains(QStringLiteral("alsa-seq"))) {
	                loaded = true;
	                break;
	              }
	            }
	            return loaded;
	          };

	          if (bridgeSub == QStringLiteral("status")) {
	            const bool enabled = AlsaSeqBridge::isConfigInstalled();
	            const bool alsaPresent = AlsaSeqBridge::alsaSequencerDevicePresent();
	            const auto moduleLoaded = moduleLoadedOpt();

	            if (jsonOutput) {
	              QJsonObject o;
	              o.insert(QStringLiteral("configInstalled"), enabled);
	              o.insert(QStringLiteral("configPath"), AlsaSeqBridge::configSnippetPath());
	              o.insert(QStringLiteral("alsaSequencerDevicePresent"), alsaPresent);
	              if (moduleLoaded.has_value()) {
	                o.insert(QStringLiteral("pipewireModuleLoaded"), *moduleLoaded);
	              } else {
	                o.insert(QStringLiteral("pipewireModuleLoaded"), QJsonValue());
	              }
	              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
	            } else {
	              out << "config\t" << (enabled ? "enabled" : "disabled") << "\n";
	              out << "path\t" << AlsaSeqBridge::configSnippetPath() << "\n";
	              out << "alsa-seq\t" << (alsaPresent ? "present" : "missing") << "\n";
	              if (moduleLoaded.has_value()) {
	                out << "module\t" << (*moduleLoaded ? "loaded" : "not-loaded") << "\n";
	              } else {
	                out << "module\tunknown (PipeWire not reachable)\n";
	              }
	            }

	            exitCode = 0;
	            break;
	          }

	          if (bridgeSub == QStringLiteral("enable")) {
	            if (args.size() < 5) {
	              err << "headroomctl: engine midi-bridge enable expects on|off|toggle\n";
	              exitCode = 2;
	              break;
	            }

	            const QString v = args.at(4).trimmed().toLower();
	            const bool cur = AlsaSeqBridge::isConfigInstalled();
	            bool target = cur;
	            QString opErr;
	            bool ok = false;

	            if (v == QStringLiteral("on")) {
	              target = true;
	              ok = AlsaSeqBridge::installConfig(&opErr);
	            } else if (v == QStringLiteral("off")) {
	              target = false;
	              ok = AlsaSeqBridge::removeConfig(&opErr);
	            } else if (v == QStringLiteral("toggle")) {
	              target = !cur;
	              ok = target ? AlsaSeqBridge::installConfig(&opErr) : AlsaSeqBridge::removeConfig(&opErr);
	            } else {
	              err << "headroomctl: engine midi-bridge enable expects on|off|toggle\n";
	              exitCode = 2;
	              break;
	            }

	            if (!ok) {
	              err << "headroomctl: failed: " << (opErr.isEmpty() ? QStringLiteral("(unknown)") : opErr) << "\n";
	              exitCode = 1;
	              break;
	            }

	            if (jsonOutput) {
	              QJsonObject o;
	              o.insert(QStringLiteral("ok"), true);
	              o.insert(QStringLiteral("enabled"), target);
	              o.insert(QStringLiteral("configPath"), AlsaSeqBridge::configSnippetPath());
	              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
	            } else {
	              out << "ok\tenabled=" << (target ? "1" : "0") << "\n";
	              out << "note\trestart PipeWire to apply\n";
	            }

	            exitCode = 0;
	            break;
	          }

	          err << "headroomctl: engine midi-bridge expects status|enable\n";
	          exitCode = 2;
	          break;
	        }

	        if (sub == QStringLiteral("clock")) {
	          const QString clockSub = (args.size() >= 4) ? args.at(3).trimmed().toLower() : QStringLiteral("status");

          PipeWireThread pw;
          PipeWireGraph graph(&pw);
          if (!pw.isConnected()) {
            err << "headroomctl: failed to connect to PipeWire\n";
            exitCode = 1;
            break;
          }

          waitForGraph(250);

          auto settingsToJson = [](const PwClockSettings& s) {
            QJsonObject o;
            if (s.rate.has_value()) {
              o.insert(QStringLiteral("rate"), static_cast<qint64>(*s.rate));
            } else {
              o.insert(QStringLiteral("rate"), QJsonValue());
            }
            if (s.quantum.has_value()) {
              o.insert(QStringLiteral("quantum"), static_cast<qint64>(*s.quantum));
            } else {
              o.insert(QStringLiteral("quantum"), QJsonValue());
            }
            if (s.minQuantum.has_value()) {
              o.insert(QStringLiteral("minQuantum"), static_cast<qint64>(*s.minQuantum));
            } else {
              o.insert(QStringLiteral("minQuantum"), QJsonValue());
            }
            if (s.maxQuantum.has_value()) {
              o.insert(QStringLiteral("maxQuantum"), static_cast<qint64>(*s.maxQuantum));
            } else {
              o.insert(QStringLiteral("maxQuantum"), QJsonValue());
            }
            if (s.forceRate.has_value()) {
              o.insert(QStringLiteral("forceRate"), static_cast<qint64>(*s.forceRate));
            } else {
              o.insert(QStringLiteral("forceRate"), QJsonValue());
            }
            if (s.forceQuantum.has_value()) {
              o.insert(QStringLiteral("forceQuantum"), static_cast<qint64>(*s.forceQuantum));
            } else {
              o.insert(QStringLiteral("forceQuantum"), QJsonValue());
            }
            QJsonArray arr;
            for (uint32_t r : s.allowedRates) {
              arr.append(static_cast<qint64>(r));
            }
            o.insert(QStringLiteral("allowedRates"), arr);
            return o;
          };

          if (clockSub == QStringLiteral("presets")) {
            const auto presets = PipeWireGraph::clockPresets();
            if (jsonOutput) {
              QJsonArray arr;
              for (const auto& p : presets) {
                QJsonObject o;
                o.insert(QStringLiteral("id"), p.id);
                o.insert(QStringLiteral("title"), p.title);
                if (p.forceRate.has_value()) {
                  o.insert(QStringLiteral("forceRate"), static_cast<qint64>(*p.forceRate));
                } else {
                  o.insert(QStringLiteral("forceRate"), QJsonValue());
                }
                if (p.forceQuantum.has_value()) {
                  o.insert(QStringLiteral("forceQuantum"), static_cast<qint64>(*p.forceQuantum));
                } else {
                  o.insert(QStringLiteral("forceQuantum"), QJsonValue());
                }
                arr.append(o);
              }
              out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              for (const auto& p : presets) {
                out << p.id << "\t" << p.title << "\n";
              }
            }
            exitCode = 0;
            break;
          }

          if (!graph.hasClockSettingsSupport()) {
            err << "headroomctl: PipeWire clock controls unavailable (settings metadata missing)\n";
            exitCode = 1;
            break;
          }

          if (clockSub == QStringLiteral("preset")) {
            if (args.size() < 5) {
              err << "headroomctl: engine clock preset expects <preset-id>\n";
              exitCode = 2;
              break;
            }
            const QString presetId = args.at(4).trimmed();
            const bool ok = graph.applyClockPreset(presetId);
            if (!ok) {
              err << "headroomctl: failed to apply preset: " << presetId << "\n";
              exitCode = 1;
              break;
            }

            waitForGraph(120);
            const PwClockSettings s = graph.clockSettings();
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("preset"), presetId);
              o.insert(QStringLiteral("clock"), settingsToJson(s));
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "ok\tpreset=" << presetId << "\n";
            }
            exitCode = 0;
            break;
          }

          if (clockSub == QStringLiteral("set")) {
            bool rateProvided = false;
            bool quantumProvided = false;
            bool minProvided = false;
            bool maxProvided = false;
            std::optional<uint32_t> rate;
            std::optional<uint32_t> quantum;
            std::optional<uint32_t> minQ;
            std::optional<uint32_t> maxQ;

            auto parseMaybeU32 = [](const QString& s, bool* okOut) -> std::optional<uint32_t> {
              const QString t = s.trimmed().toLower();
              if (t == QStringLiteral("auto") || t == QStringLiteral("off")) {
                if (okOut) {
                  *okOut = true;
                }
                return std::nullopt;
              }
              bool ok = false;
              const uint32_t v = t.toUInt(&ok);
              if (okOut) {
                *okOut = ok;
              }
              return ok ? std::optional<uint32_t>(v) : std::nullopt;
            };

            for (int i = 4; i < args.size(); ++i) {
              const QString a = args.at(i);
              auto takeValue = [&](QString* outValue) -> bool {
                const int eq = a.indexOf('=');
                if (eq >= 0) {
                  *outValue = a.mid(eq + 1);
                  return true;
                }
                if (i + 1 >= args.size()) {
                  return false;
                }
                *outValue = args.at(++i);
                return true;
              };

              QString v;
              if (a == QStringLiteral("--rate") || a.startsWith(QStringLiteral("--rate="))) {
                if (!takeValue(&v)) {
                  err << "headroomctl: --rate expects a value\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                bool ok = false;
                rate = parseMaybeU32(v, &ok);
                if (!ok) {
                  err << "headroomctl: invalid --rate value: " << v << "\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                rateProvided = true;
                continue;
              }

              if (a == QStringLiteral("--quantum") || a.startsWith(QStringLiteral("--quantum="))) {
                if (!takeValue(&v)) {
                  err << "headroomctl: --quantum expects a value\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                bool ok = false;
                quantum = parseMaybeU32(v, &ok);
                if (!ok) {
                  err << "headroomctl: invalid --quantum value: " << v << "\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                quantumProvided = true;
                continue;
              }

              if (a == QStringLiteral("--min-quantum") || a.startsWith(QStringLiteral("--min-quantum="))) {
                if (!takeValue(&v)) {
                  err << "headroomctl: --min-quantum expects a value\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                bool ok = false;
                minQ = parseMaybeU32(v, &ok);
                if (!ok) {
                  err << "headroomctl: invalid --min-quantum value: " << v << "\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                minProvided = true;
                continue;
              }

              if (a == QStringLiteral("--max-quantum") || a.startsWith(QStringLiteral("--max-quantum="))) {
                if (!takeValue(&v)) {
                  err << "headroomctl: --max-quantum expects a value\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                bool ok = false;
                maxQ = parseMaybeU32(v, &ok);
                if (!ok) {
                  err << "headroomctl: invalid --max-quantum value: " << v << "\n";
                  exitCode = 2;
                  goto engine_clock_set_done;
                }
                maxProvided = true;
                continue;
              }

              err << "headroomctl: unknown option: " << a << "\n";
              exitCode = 2;
              goto engine_clock_set_done;
            }

            {
              bool okAll = true;
              if (rateProvided) {
                okAll = okAll && graph.setClockForceRate(rate);
              }
              if (quantumProvided) {
                okAll = okAll && graph.setClockForceQuantum(quantum);
              }
              if (minProvided) {
                okAll = okAll && graph.setClockMinQuantum(minQ);
              }
              if (maxProvided) {
                okAll = okAll && graph.setClockMaxQuantum(maxQ);
              }
              waitForGraph(120);
              const PwClockSettings s = graph.clockSettings();

              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), okAll);
                QJsonObject applied;
                if (rateProvided) {
                  applied.insert(QStringLiteral("rate"), rate.has_value() ? QJsonValue(static_cast<qint64>(*rate)) : QJsonValue());
                }
                if (quantumProvided) {
                  applied.insert(QStringLiteral("quantum"), quantum.has_value() ? QJsonValue(static_cast<qint64>(*quantum)) : QJsonValue());
                }
                if (minProvided) {
                  applied.insert(QStringLiteral("minQuantum"), minQ.has_value() ? QJsonValue(static_cast<qint64>(*minQ)) : QJsonValue());
                }
                if (maxProvided) {
                  applied.insert(QStringLiteral("maxQuantum"), maxQ.has_value() ? QJsonValue(static_cast<qint64>(*maxQ)) : QJsonValue());
                }
                o.insert(QStringLiteral("applied"), applied);
                o.insert(QStringLiteral("clock"), settingsToJson(s));
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << (okAll ? "ok" : "failed") << "\n";
              }

              exitCode = okAll ? 0 : 1;
            }

          engine_clock_set_done:
            break;
          }

          // Default: status
          if (clockSub == QStringLiteral("status")) {
            const PwClockSettings s = graph.clockSettings();
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("clock"), settingsToJson(s));
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              const uint32_t rateNow = s.rate.value_or(0);
              const uint32_t quantumNow = s.quantum.value_or(0);
              out << "rate\t" << (rateNow > 0 ? QString::number(rateNow) : QStringLiteral("-")) << "\n";
              out << "quantum\t" << (quantumNow > 0 ? QString::number(quantumNow) : QStringLiteral("-")) << "\n";
              out << "force-rate\t" << (s.forceRate.has_value() ? QString::number(*s.forceRate) : QStringLiteral("auto")) << "\n";
              out << "force-quantum\t" << (s.forceQuantum.has_value() ? QString::number(*s.forceQuantum) : QStringLiteral("auto")) << "\n";
              out << "min-quantum\t" << (s.minQuantum.has_value() ? QString::number(*s.minQuantum) : QStringLiteral("-")) << "\n";
              out << "max-quantum\t" << (s.maxQuantum.has_value() ? QString::number(*s.maxQuantum) : QStringLiteral("-")) << "\n";
            }
            exitCode = 0;
            break;
          }

          err << "headroomctl: engine clock expects status|presets|preset|set\n";
          exitCode = 2;
          break;
        }

        if (sub == QStringLiteral("status")) {
          const QStringList units = EngineControl::defaultUserUnits();
          QVector<SystemdUnitStatus> statuses;
          statuses.reserve(units.size());
          for (const auto& u : units) {
            statuses.push_back(EngineControl::queryUserUnit(u));
          }

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), userSystemdOk);
            o.insert(QStringLiteral("systemctlAvailable"), systemctlOk);
            o.insert(QStringLiteral("userSystemdAvailable"), userSystemdOk);
            if (!systemdErr.isEmpty()) {
              o.insert(QStringLiteral("error"), systemdErr);
            }
            QJsonArray arr;
            for (const auto& st : statuses) {
              arr.append(statusToJson(st));
            }
            o.insert(QStringLiteral("units"), arr);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            if (!systemctlOk) {
              out << "systemctl\tmissing\n";
              exitCode = 1;
              break;
            }
            if (!userSystemdOk) {
              out << "systemd-user\tunavailable\t" << systemdErr << "\n";
              exitCode = 1;
              break;
            }
            for (const auto& st : statuses) {
              out << st.unit << "\t" << (st.exists() ? st.loadState : QStringLiteral("not-found")) << "\t" << st.activeState << "\t"
                  << st.subState << "\t" << st.description << "\n";
            }
          }

          exitCode = userSystemdOk ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("start") || sub == QStringLiteral("stop") || sub == QStringLiteral("restart")) {
          if (args.size() < 4) {
            err << "headroomctl: engine " << sub << " expects <unit|all>\n";
            exitCode = 2;
            break;
          }
          if (!systemctlOk) {
            err << "headroomctl: systemctl not found\n";
            exitCode = 1;
            break;
          }
          if (!userSystemdOk) {
            err << "headroomctl: systemd user instance unavailable: " << systemdErr << "\n";
            exitCode = 1;
            break;
          }

          const QString target = args.at(3).trimmed();
          QStringList units;
          if (target.toLower() == QStringLiteral("all")) {
            units = EngineControl::defaultUserUnits();
          } else {
            units = QStringList{EngineControl::normalizeUnitAlias(target)};
          }

          bool allOk = true;
          QJsonArray results;
          QStringList lines;

          for (const auto& u : units) {
            QString actionErr;
            bool ok = false;
            if (sub == QStringLiteral("start")) {
              ok = EngineControl::startUserUnit(u, &actionErr);
            } else if (sub == QStringLiteral("stop")) {
              ok = EngineControl::stopUserUnit(u, &actionErr);
            } else if (sub == QStringLiteral("restart")) {
              ok = EngineControl::restartUserUnit(u, &actionErr);
            }

            const SystemdUnitStatus after = EngineControl::queryUserUnit(u);

            if (jsonOutput) {
              QJsonObject r;
              r.insert(QStringLiteral("unit"), u);
              r.insert(QStringLiteral("action"), sub);
              r.insert(QStringLiteral("ok"), ok);
              if (!actionErr.isEmpty()) {
                r.insert(QStringLiteral("error"), actionErr);
              }
              r.insert(QStringLiteral("status"), statusToJson(after));
              results.append(r);
            } else {
              QString line = QStringLiteral("%1\t%2").arg(u, ok ? QStringLiteral("ok") : QStringLiteral("failed"));
              if (!actionErr.isEmpty()) {
                line += QStringLiteral("\t%1").arg(actionErr);
              }
              lines.push_back(line);
            }

            if (!ok) {
              allOk = false;
            }
          }

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), allOk);
            o.insert(QStringLiteral("results"), results);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& l : lines) {
              out << l << "\n";
            }
          }

          exitCode = allOk ? 0 : 1;
          break;
        }

        err << "headroomctl: engine expects status|start|stop|restart|midi-bridge|clock\n";
        exitCode = 2;
        break;
      }

      PipeWireThread pw;
      PipeWireGraph graph(&pw);

      if (!pw.isConnected()) {
        err << "headroomctl: failed to connect to PipeWire\n";
        exitCode = 1;
        break;
      }

      waitForGraph(250);
      if (graph.nodes().isEmpty()) {
        err << "headroomctl: warning: no PipeWire nodes found (is PipeWire running? is XDG_RUNTIME_DIR set?)\n";
      }

      if (cmd == QStringLiteral("diagnostics")) {
        const QString sub = (args.size() >= 3) ? args.at(2).trimmed().toLower() : QStringLiteral("status");
        if (sub != QStringLiteral("status") && sub != QStringLiteral("drivers")) {
          err << "headroomctl: diagnostics expects status|drivers\n";
          exitCode = 2;
          break;
        }

        if (!graph.hasProfilerSupport()) {
          err << "headroomctl: profiler unavailable (PipeWire module-profiler not loaded)\n";
          exitCode = 1;
          break;
        }

        QElapsedTimer t;
        t.start();
        std::optional<PwProfilerSnapshot> snap;
        while (t.elapsed() < 800) {
          snap = graph.profilerSnapshot();
          if (snap.has_value() && snap->seq > 0) {
            break;
          }
          QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
          QThread::msleep(10);
        }

        const bool haveData = snap.has_value() && snap->seq > 0;
        const PwProfilerSnapshot s = haveData ? *snap : PwProfilerSnapshot{};

        auto blockToJson = [](const PwProfilerBlock& b) {
          QJsonObject o;
          o.insert(QStringLiteral("id"), static_cast<qint64>(b.id));
          o.insert(QStringLiteral("name"), b.name);
          o.insert(QStringLiteral("status"), b.status);
          o.insert(QStringLiteral("xrunCount"), b.xrunCount);
          o.insert(QStringLiteral("latencyMs"), b.latencyMs.has_value() ? QJsonValue(*b.latencyMs) : QJsonValue());
          o.insert(QStringLiteral("waitMs"), b.waitMs.has_value() ? QJsonValue(*b.waitMs) : QJsonValue());
          o.insert(QStringLiteral("busyMs"), b.busyMs.has_value() ? QJsonValue(*b.busyMs) : QJsonValue());
          o.insert(QStringLiteral("waitRatio"), b.waitRatio.has_value() ? QJsonValue(*b.waitRatio) : QJsonValue());
          o.insert(QStringLiteral("busyRatio"), b.busyRatio.has_value() ? QJsonValue(*b.busyRatio) : QJsonValue());
          return o;
        };

        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("seq"), static_cast<qint64>(s.seq));

          if (sub == QStringLiteral("status")) {
            QJsonObject info;
            info.insert(QStringLiteral("available"), s.hasInfo);
            info.insert(QStringLiteral("counter"), static_cast<qint64>(s.counter));
            info.insert(QStringLiteral("cpuLoadFast"), s.cpuLoadFast);
            info.insert(QStringLiteral("cpuLoadMedium"), s.cpuLoadMedium);
            info.insert(QStringLiteral("cpuLoadSlow"), s.cpuLoadSlow);
            info.insert(QStringLiteral("xrunCount"), s.xrunCount);
            o.insert(QStringLiteral("info"), info);

            QJsonObject clock;
            clock.insert(QStringLiteral("available"), s.hasClock);
            clock.insert(QStringLiteral("cycle"), s.clockCycle);
            clock.insert(QStringLiteral("durationMs"), s.clockDurationMs.has_value() ? QJsonValue(*s.clockDurationMs) : QJsonValue());
            clock.insert(QStringLiteral("delayMs"), s.clockDelayMs.has_value() ? QJsonValue(*s.clockDelayMs) : QJsonValue());
            clock.insert(QStringLiteral("xrunDurationMs"), s.clockXrunDurationMs.has_value() ? QJsonValue(*s.clockXrunDurationMs) : QJsonValue());
            o.insert(QStringLiteral("clock"), clock);
          }

          QJsonArray drivers;
          for (const auto& d : s.drivers) {
            drivers.append(blockToJson(d));
          }
          o.insert(QStringLiteral("drivers"), drivers);

          QJsonArray followers;
          for (const auto& f : s.followers) {
            followers.append(blockToJson(f));
          }
          o.insert(QStringLiteral("followers"), followers);

          if (!haveData) {
            o.insert(QStringLiteral("note"), QStringLiteral("no profiler data received (graph may be idle; start audio to activate drivers)"));
          }

          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          auto fmtPct = [](double ratio) -> QString {
            const double pct = ratio * 100.0;
            if (!std::isfinite(pct)) {
              return QStringLiteral("-");
            }
            return QString::number(pct, 'f', pct < 10.0 ? 2 : 1) + "%";
          };

          auto fmtMs = [](const std::optional<double>& ms) -> QString {
            if (!ms.has_value() || !std::isfinite(*ms)) {
              return QStringLiteral("-");
            }
            return QString::number(*ms, 'f', (*ms < 10.0) ? 2 : 1);
          };

          if (sub == QStringLiteral("status")) {
            if (s.hasInfo) {
              out << "cpu-fast\t" << fmtPct(s.cpuLoadFast) << "\n";
              out << "cpu-med\t" << fmtPct(s.cpuLoadMedium) << "\n";
              out << "cpu-slow\t" << fmtPct(s.cpuLoadSlow) << "\n";
              out << "xruns\t" << s.xrunCount << "\n";
            } else {
              out << "info\twaiting\n";
              if (!haveData) {
                out << "note\tno profiler data received (graph may be idle; start audio to activate drivers)\n";
              }
            }

            if (s.hasClock) {
              if (s.clockDurationMs.has_value()) {
                out << "clock-duration-ms\t" << fmtMs(s.clockDurationMs) << "\n";
              }
              if (s.clockDelayMs.has_value()) {
                out << "clock-delay-ms\t" << fmtMs(s.clockDelayMs) << "\n";
              }
              if (s.clockXrunDurationMs.has_value()) {
                out << "clock-xrun-ms\t" << fmtMs(s.clockXrunDurationMs) << "\n";
              }
              if (s.clockCycle > 0) {
                out << "clock-cycle\t" << s.clockCycle << "\n";
              }
            }
          }

          for (const auto& d : s.drivers) {
            out << "driver\t" << d.id << "\t" << (d.name.isEmpty() ? QStringLiteral("(unnamed)") : d.name) << "\tlat-ms=" << fmtMs(d.latencyMs)
                << "\tbusy=" << (d.busyRatio.has_value() ? fmtPct(*d.busyRatio) : QStringLiteral("-"))
                << "\twait=" << (d.waitRatio.has_value() ? fmtPct(*d.waitRatio) : QStringLiteral("-")) << "\txruns=" << d.xrunCount << "\n";
          }
        }

        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("nodes")) {
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& n : graph.nodes()) {
            arr.append(nodeToJson(n));
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          printAllNodes(out, graph.nodes());
        }
        exitCode = 0;
        break;
      }
      if (cmd == QStringLiteral("sinks")) {
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& n : graph.audioSinks()) {
            QJsonObject o = nodeToJson(n);
            o.insert(QStringLiteral("controls"), nodeControlsToJson(graph.nodeControls(n.id).value_or(PwNodeControls{})));
            arr.append(o);
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          printNodes(out, graph.audioSinks(), graph);
        }
        exitCode = 0;
        break;
      }
      if (cmd == QStringLiteral("sources")) {
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& n : graph.audioSources()) {
            QJsonObject o = nodeToJson(n);
            o.insert(QStringLiteral("controls"), nodeControlsToJson(graph.nodeControls(n.id).value_or(PwNodeControls{})));
            arr.append(o);
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          printNodes(out, graph.audioSources(), graph);
        }
        exitCode = 0;
        break;
      }
      if (cmd == QStringLiteral("default-sink") || cmd == QStringLiteral("default-source")) {
        const bool wantSink = (cmd == QStringLiteral("default-sink"));
        const QString sub = (args.size() >= 3) ? args.at(2).trimmed().toLower() : QStringLiteral("get");

        auto resolveDefaultNodeId = [&](const QString& idOrName) -> std::optional<uint32_t> {
          const QString key = idOrName.trimmed();
          bool ok = false;
          const uint32_t id = key.toUInt(&ok);
          if (ok) {
            return id;
          }

          const QList<PwNodeInfo> candidates = wantSink ? graph.audioSinks() : graph.audioSources();
          for (const auto& n : candidates) {
            if (n.name == key || n.description == key || nodeLabel(n) == key) {
              return n.id;
            }
          }
          return std::nullopt;
        };

        auto printCurrent = [&]() {
          const std::optional<uint32_t> idOpt = wantSink ? graph.defaultAudioSinkId() : graph.defaultAudioSourceId();
          const PwNodeInfo n = idOpt.has_value() ? graph.nodeById(*idOpt).value_or(PwNodeInfo{}) : PwNodeInfo{};

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            if (wantSink) {
              o.insert(QStringLiteral("defaultSinkId"), idOpt.has_value() ? QJsonValue(static_cast<qint64>(*idOpt)) : QJsonValue());
              if (n.id != 0u) {
                o.insert(QStringLiteral("defaultSink"), nodeToJson(n));
              } else {
                o.insert(QStringLiteral("defaultSink"), QJsonValue());
              }
            } else {
              o.insert(QStringLiteral("defaultSourceId"), idOpt.has_value() ? QJsonValue(static_cast<qint64>(*idOpt)) : QJsonValue());
              if (n.id != 0u) {
                o.insert(QStringLiteral("defaultSource"), nodeToJson(n));
              } else {
                o.insert(QStringLiteral("defaultSource"), QJsonValue());
              }
            }
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            if (!idOpt.has_value()) {
              out << "none\n";
            } else {
              out << *idOpt << "\t" << nodeLabel(n) << "\n";
            }
          }
        };

        if (sub == QStringLiteral("get")) {
          printCurrent();
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("set")) {
          if (args.size() < 4) {
            err << "headroomctl: " << cmd << " set expects <node-id|node-name>\n";
            exitCode = 2;
            break;
          }
          if (!graph.hasDefaultDeviceSupport()) {
            err << "headroomctl: default device controls unavailable (settings metadata missing)\n";
            exitCode = 1;
            break;
          }

          const auto idOpt = resolveDefaultNodeId(args.at(3));
          if (!idOpt.has_value()) {
            err << "headroomctl: unknown node (expected a sink/source id or name)\n";
            exitCode = 2;
            break;
          }

          bool ok = false;
          QElapsedTimer t;
          t.start();
          while (t.elapsed() < 1500) {
            ok = wantSink ? graph.setDefaultAudioSink(*idOpt) : graph.setDefaultAudioSource(*idOpt);
            if (ok) {
              break;
            }
            waitForGraph(60);
          }
          if (!ok) {
            err << "headroomctl: failed to set default device\n";
            exitCode = 1;
            break;
          }

          waitForGraph(120);
          printCurrent();
          exitCode = 0;
          break;
        }

        err << "headroomctl: " << cmd << " expects [get] or set <node-id|node-name>\n";
        exitCode = 2;
        break;
      }
      if (cmd == QStringLiteral("ports")) {
        const QList<PwPortInfo> ports = graph.ports();
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& p : ports) {
            arr.append(portToJson(p, graph.nodeById(p.nodeId)));
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          QList<PwPortInfo> sorted = ports;
          std::sort(sorted.begin(), sorted.end(), [](const PwPortInfo& a, const PwPortInfo& b) {
            if (a.nodeId != b.nodeId) {
              return a.nodeId < b.nodeId;
            }
            if (a.direction != b.direction) {
              return a.direction < b.direction;
            }
            return a.name.toLower() < b.name.toLower();
          });

          for (const auto& p : sorted) {
            const PwNodeInfo n = graph.nodeById(p.nodeId).value_or(PwNodeInfo{});
            const QString label = p.alias.isEmpty() ? p.name : p.alias;
            out << p.id << "\t" << p.nodeId << "\t" << p.direction << "\t" << label << "\t" << nodeLabel(n) << "\n";
          }
        }
        exitCode = 0;
        break;
      }
      if (cmd == QStringLiteral("links")) {
        const QList<PwLinkInfo> links = graph.links();
        const QList<PwPortInfo> ports = graph.ports();
        if (jsonOutput) {
          QJsonArray arr;
          for (const auto& l : links) {
            const auto outPort = portById(ports, l.outputPortId);
            const auto inPort = portById(ports, l.inputPortId);
            arr.append(linkToJson(l, graph.nodeById(l.outputNodeId), outPort, graph.nodeById(l.inputNodeId), inPort));
          }
          out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          QList<PwLinkInfo> sorted = links;
          std::sort(sorted.begin(), sorted.end(), [](const PwLinkInfo& a, const PwLinkInfo& b) { return a.id < b.id; });

          for (const auto& l : sorted) {
            const PwNodeInfo outNode = graph.nodeById(l.outputNodeId).value_or(PwNodeInfo{});
            const PwNodeInfo inNode = graph.nodeById(l.inputNodeId).value_or(PwNodeInfo{});
            const PwPortInfo outPort = portById(ports, l.outputPortId).value_or(PwPortInfo{});
            const PwPortInfo inPort = portById(ports, l.inputPortId).value_or(PwPortInfo{});

            out << l.id << "\t" << l.outputPortId << "\t" << l.inputPortId << "\t" << nodeLabel(outNode) << ":" << outPort.name << " -> "
                << nodeLabel(inNode) << ":" << inPort.name << "\n";
          }
        }
        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("patchbay")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        const bool strict = args.contains(QStringLiteral("--strict"));
        if (strict) {
          args.removeAll(QStringLiteral("--strict"));
        }
        const bool noHooks = args.contains(QStringLiteral("--no-hooks"));
        if (noHooks) {
          args.removeAll(QStringLiteral("--no-hooks"));
        }

        QSettings s;

        if (sub == QStringLiteral("profiles")) {
          const QStringList names = PatchbayProfileStore::listProfileNames(s);
          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& name : names) {
              arr.append(name);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& name : names) {
              out << name << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("hooks")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString action = args.at(3).trimmed().toLower();
          const QString profileName = args.at(4).trimmed();
          if (profileName.isEmpty()) {
            err << "headroomctl: patchbay hooks expects <profile-name>\n";
            exitCode = 2;
            break;
          }

          if (action == QStringLiteral("get")) {
            const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("name"), profileName);
              o.insert(QStringLiteral("onLoad"), h.onLoadCommand);
              o.insert(QStringLiteral("onUnload"), h.onUnloadCommand);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "profile\t" << profileName << "\n";
              out << "on-load\t" << h.onLoadCommand << "\n";
              out << "on-unload\t" << h.onUnloadCommand << "\n";
            }
            exitCode = 0;
            break;
          }

          if (action == QStringLiteral("set")) {
            if (args.size() < 7) {
              err << "headroomctl: patchbay hooks set expects <profile-name> load|unload <command...>\n";
              exitCode = 2;
              break;
            }
            const QString which = args.at(5).trimmed().toLower();
            const QString cmdText = args.mid(6).join(QLatin1Char(' ')).trimmed();

            PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
            if (which == QStringLiteral("load") || which == QStringLiteral("onload")) {
              h.onLoadCommand = cmdText;
            } else if (which == QStringLiteral("unload") || which == QStringLiteral("onunload")) {
              h.onUnloadCommand = cmdText;
            } else {
              err << "headroomctl: patchbay hooks set expects load|unload\n";
              exitCode = 2;
              break;
            }

            PatchbayProfileHooksStore::save(s, profileName, h);
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("name"), profileName);
              o.insert(QStringLiteral("onLoad"), h.onLoadCommand);
              o.insert(QStringLiteral("onUnload"), h.onUnloadCommand);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "set\t" << which << "\t" << profileName << "\n";
            }
            exitCode = 0;
            break;
          }

          if (action == QStringLiteral("clear")) {
            if (args.size() < 6) {
              err << "headroomctl: patchbay hooks clear expects <profile-name> load|unload|all\n";
              exitCode = 2;
              break;
            }
            const QString which = args.at(5).trimmed().toLower();
            if (which == QStringLiteral("all")) {
              PatchbayProfileHooksStore::clear(s, profileName);
            } else {
              PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, profileName);
              if (which == QStringLiteral("load") || which == QStringLiteral("onload")) {
                h.onLoadCommand.clear();
              } else if (which == QStringLiteral("unload") || which == QStringLiteral("onunload")) {
                h.onUnloadCommand.clear();
              } else {
                err << "headroomctl: patchbay hooks clear expects load|unload|all\n";
                exitCode = 2;
                break;
              }
              PatchbayProfileHooksStore::save(s, profileName, h);
            }

            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("name"), profileName);
              o.insert(QStringLiteral("cleared"), which);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "cleared\t" << which << "\t" << profileName << "\n";
            }
            exitCode = 0;
            break;
          }

          err << "headroomctl: patchbay hooks expects get|set|clear\n";
          exitCode = 2;
          break;
        }

        if (sub == QStringLiteral("save")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay save expects <profile-name>\n";
            exitCode = 2;
            break;
          }

          const PatchbayProfile profile = snapshotPatchbayProfile(name, graph);
          PatchbayProfileStore::save(s, profile);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("name"), profile.name);
            o.insert(QStringLiteral("links"), static_cast<int>(profile.links.size()));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "saved\t" << profile.name << "\tlinks=" << profile.links.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("apply")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay apply expects <profile-name>\n";
            exitCode = 2;
            break;
          }

          const QString prevActive = s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed();

          const auto profile = PatchbayProfileStore::load(s, name);
          if (!profile) {
            err << "headroomctl: patchbay profile not found: " << name << "\n";
            exitCode = 1;
            break;
          }

          PatchbayProfileHookStartResult unloadHook;
          if (!noHooks && !prevActive.isEmpty() && prevActive != profile->name) {
            const PatchbayProfileHooks h = PatchbayProfileHooksStore::load(s, prevActive);
            unloadHook = startPatchbayProfileHookDetached(prevActive,
                                                         QString{},
                                                         profile->name,
                                                         PatchbayProfileHookEvent::Unload,
                                                         h.onUnloadCommand);
          }

          const PatchbayProfileApplyResult r = applyPatchbayProfile(graph, *profile, strict);

          PatchbayProfileHookStartResult loadHook;
          if (!noHooks) {
            const PatchbayProfileHooks h2 = PatchbayProfileHooksStore::load(s, profile->name);
            loadHook = startPatchbayProfileHookDetached(profile->name,
                                                       prevActive,
                                                       QString{},
                                                       PatchbayProfileHookEvent::Load,
                                                       h2.onLoadCommand);
          }

          s.setValue(SettingsKeys::patchbaySelectedProfileName(), profile->name);
          s.setValue(SettingsKeys::patchbayActiveProfileName(), profile->name);

          if (jsonOutput) {
            QJsonObject o;
            const bool hookOk = unloadHook.error.isEmpty() && loadHook.error.isEmpty();
            o.insert(QStringLiteral("ok"), r.errors.isEmpty() && hookOk);
            o.insert(QStringLiteral("name"), profile->name);
            o.insert(QStringLiteral("strict"), strict);
            o.insert(QStringLiteral("hooksEnabled"), !noHooks);
            o.insert(QStringLiteral("prevActiveProfile"), prevActive);
            o.insert(QStringLiteral("desiredLinks"), r.desiredLinks);
            o.insert(QStringLiteral("createdLinks"), r.createdLinks);
            o.insert(QStringLiteral("alreadyPresentLinks"), r.alreadyPresentLinks);
            o.insert(QStringLiteral("disconnectedLinks"), r.disconnectedLinks);
            o.insert(QStringLiteral("missingEndpoints"), r.missingEndpoints);
            QJsonObject hooks;
            hooks.insert(QStringLiteral("unloadStarted"), unloadHook.started);
            hooks.insert(QStringLiteral("unloadPid"), static_cast<qint64>(unloadHook.pid));
            hooks.insert(QStringLiteral("unloadError"), unloadHook.error);
            hooks.insert(QStringLiteral("loadStarted"), loadHook.started);
            hooks.insert(QStringLiteral("loadPid"), static_cast<qint64>(loadHook.pid));
            hooks.insert(QStringLiteral("loadError"), loadHook.error);
            o.insert(QStringLiteral("hooks"), hooks);
            QJsonArray missing;
            for (const auto& m : r.missing) {
              missing.append(m);
            }
            QJsonArray errors;
            for (const auto& e : r.errors) {
              errors.append(e);
            }
            o.insert(QStringLiteral("missing"), missing);
            o.insert(QStringLiteral("errors"), errors);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "applied\t" << profile->name << "\tcreated=" << r.createdLinks << "\talready=" << r.alreadyPresentLinks;
            if (strict) {
              out << "\tdisconnected=" << r.disconnectedLinks;
            }
            out << "\tmissing=" << r.missingEndpoints << "\terrors=" << r.errors.size() << "\n";
            for (const auto& e : r.errors) {
              err << "error:\t" << e << "\n";
            }
            if (!noHooks) {
              if (unloadHook.started) {
                out << "hook\tunload\tpid=" << unloadHook.pid << "\tprofile=" << prevActive << "\n";
              } else if (!unloadHook.error.isEmpty()) {
                err << "hook-error:\tunload\t" << unloadHook.error << "\tprofile=" << prevActive << "\n";
              }
              if (loadHook.started) {
                out << "hook\tload\tpid=" << loadHook.pid << "\tprofile=" << profile->name << "\n";
              } else if (!loadHook.error.isEmpty()) {
                err << "hook-error:\tload\t" << loadHook.error << "\tprofile=" << profile->name << "\n";
              }
            }
          }
          exitCode = (r.errors.isEmpty() && unloadHook.error.isEmpty() && loadHook.error.isEmpty()) ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("delete")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = args.at(3).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: patchbay delete expects <profile-name>\n";
            exitCode = 2;
            break;
          }
          const bool removed = PatchbayProfileStore::remove(s, name);
          if (removed) {
            PatchbayProfileHooksStore::clear(s, name);
            if (s.value(SettingsKeys::patchbaySelectedProfileName()).toString().trimmed() == name) {
              s.remove(SettingsKeys::patchbaySelectedProfileName());
            }
            if (s.value(SettingsKeys::patchbayActiveProfileName()).toString().trimmed() == name) {
              s.remove(SettingsKeys::patchbayActiveProfileName());
            }
          }
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), removed);
            o.insert(QStringLiteral("name"), name);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << (removed ? "deleted" : "not-found") << "\t" << name << "\n";
          }
          exitCode = removed ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("port")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString sub2 = args.at(3).trimmed().toLower();

          auto loadEndpoint = [&](uint32_t portId, PwPortInfo* outPort, PwNodeInfo* outNode) -> bool {
            const auto portOpt = portById(graph.ports(), portId);
            if (!portOpt) {
              return false;
            }
            const PwPortInfo port = *portOpt;
            const PwNodeInfo node = graph.nodeById(port.nodeId).value_or(PwNodeInfo{});
            if (outPort) {
              *outPort = port;
            }
            if (outNode) {
              *outNode = node;
            }
            return true;
          };

          if (sub2 == QStringLiteral("status")) {
            const auto portId = parseNodeId(args.at(4));
            if (!portId) {
              err << "headroomctl: patchbay port status expects <port-id>\n";
              exitCode = 2;
              break;
            }

            PwPortInfo port;
            PwNodeInfo node;
            if (!loadEndpoint(*portId, &port, &node)) {
              err << "headroomctl: unknown port id\n";
              exitCode = 2;
              break;
            }

            const PatchbayPortConfig cfg = PatchbayPortConfigStore::load(s, node.name, port.name);

            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("portId"), static_cast<qint64>(port.id));
              o.insert(QStringLiteral("nodeId"), static_cast<qint64>(port.nodeId));
              o.insert(QStringLiteral("nodeName"), node.name);
              o.insert(QStringLiteral("portName"), port.name);
              o.insert(QStringLiteral("customAlias"), cfg.customAlias.value_or(QString{}));
              o.insert(QStringLiteral("locked"), cfg.locked);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "port\t" << port.id << "\n";
              out << "node\t" << node.name << "\n";
              out << "name\t" << port.name << "\n";
              out << "custom-alias\t" << cfg.customAlias.value_or(QStringLiteral("")) << "\n";
              out << "locked\t" << (cfg.locked ? "yes" : "no") << "\n";
            }
            exitCode = 0;
            break;
          }

          if (sub2 == QStringLiteral("alias")) {
            if (args.size() < 6) {
              err << "headroomctl: patchbay port alias expects get|set|clear <port-id> ...\n";
              exitCode = 2;
              break;
            }
            const QString op = args.at(4).trimmed().toLower();
            const auto portId = parseNodeId(args.at(5));
            if (!portId) {
              err << "headroomctl: patchbay port alias expects <port-id>\n";
              exitCode = 2;
              break;
            }

            PwPortInfo port;
            PwNodeInfo node;
            if (!loadEndpoint(*portId, &port, &node)) {
              err << "headroomctl: unknown port id\n";
              exitCode = 2;
              break;
            }

            if (op == QStringLiteral("get")) {
              const auto a = PatchbayPortConfigStore::customAlias(s, node.name, port.name);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("portId"), static_cast<qint64>(port.id));
                o.insert(QStringLiteral("nodeName"), node.name);
                o.insert(QStringLiteral("portName"), port.name);
                if (a) {
                  o.insert(QStringLiteral("customAlias"), *a);
                } else {
                  o.insert(QStringLiteral("customAlias"), QJsonValue::Null);
                }
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else if (a) {
                out << *a << "\n";
              }
              exitCode = 0;
              break;
            }

            if (op == QStringLiteral("set")) {
              if (args.size() < 7) {
                err << "headroomctl: patchbay port alias set expects <port-id> <alias>\n";
                exitCode = 2;
                break;
              }
              const QString alias = args.mid(6).join(' ').trimmed();
              PatchbayPortConfigStore::setCustomAlias(s, node.name, port.name, alias);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("customAlias"), alias);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << "alias\t" << alias << "\n";
              }
              exitCode = 0;
              break;
            }

            if (op == QStringLiteral("clear")) {
              PatchbayPortConfigStore::clearCustomAlias(s, node.name, port.name);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << "cleared\n";
              }
              exitCode = 0;
              break;
            }

            err << "headroomctl: patchbay port alias expects get|set|clear\n";
            exitCode = 2;
            break;
          }

          if (sub2 == QStringLiteral("lock")) {
            if (args.size() < 6) {
              err << "headroomctl: patchbay port lock expects get|set <port-id> ...\n";
              exitCode = 2;
              break;
            }
            const QString op = args.at(4).trimmed().toLower();
            const auto portId = parseNodeId(args.at(5));
            if (!portId) {
              err << "headroomctl: patchbay port lock expects <port-id>\n";
              exitCode = 2;
              break;
            }

            PwPortInfo port;
            PwNodeInfo node;
            if (!loadEndpoint(*portId, &port, &node)) {
              err << "headroomctl: unknown port id\n";
              exitCode = 2;
              break;
            }

            if (op == QStringLiteral("get")) {
              const bool locked = PatchbayPortConfigStore::isLocked(s, node.name, port.name);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("locked"), locked);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << (locked ? "locked" : "unlocked") << "\n";
              }
              exitCode = 0;
              break;
            }

            if (op == QStringLiteral("set")) {
              if (args.size() < 7) {
                err << "headroomctl: patchbay port lock set expects <port-id> on|off|toggle\n";
                exitCode = 2;
                break;
              }
              const QString mode = args.at(6).trimmed().toLower();
              bool target = PatchbayPortConfigStore::isLocked(s, node.name, port.name);
              if (mode == QStringLiteral("on")) {
                target = true;
              } else if (mode == QStringLiteral("off")) {
                target = false;
              } else if (mode == QStringLiteral("toggle")) {
                target = !target;
              } else {
                err << "headroomctl: patchbay port lock set expects on|off|toggle\n";
                exitCode = 2;
                break;
              }
              PatchbayPortConfigStore::setLocked(s, node.name, port.name, target);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("locked"), target);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << "locked\t" << (target ? "yes" : "no") << "\n";
              }
              exitCode = 0;
              break;
            }

            err << "headroomctl: patchbay port lock expects get|set\n";
            exitCode = 2;
            break;
          }

          err << "headroomctl: patchbay port expects status|alias|lock\n";
          exitCode = 2;
          break;
        }

        if (sub == QStringLiteral("autoconnect")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          QStringList rest = args.mid(3);
          const QString sub2 = rest.value(0).trimmed().toLower();

          auto loadCfg = [&]() {
            AutoConnectConfig cfg = loadAutoConnectConfig(s);
            return cfg;
          };

          if (sub2 == QStringLiteral("status")) {
            const AutoConnectConfig cfg = loadCfg();
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("enabled"), cfg.enabled);
              o.insert(QStringLiteral("rules"), static_cast<int>(cfg.rules.size()));
              o.insert(QStringLiteral("whitelist"), static_cast<int>(cfg.whitelist.size()));
              o.insert(QStringLiteral("blacklist"), static_cast<int>(cfg.blacklist.size()));
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "enabled\t" << (cfg.enabled ? "on" : "off") << "\n";
              out << "rules\t" << cfg.rules.size() << "\n";
              out << "whitelist\t" << cfg.whitelist.size() << "\n";
              out << "blacklist\t" << cfg.blacklist.size() << "\n";
            }
            exitCode = 0;
            break;
          }

          if (sub2 == QStringLiteral("enable")) {
            if (rest.size() < 2) {
              err << "headroomctl: patchbay autoconnect enable expects on|off|toggle\n";
              exitCode = 2;
              break;
            }
            AutoConnectConfig cfg = loadCfg();
            const QString mode = rest.value(1).trimmed().toLower();
            if (mode == QStringLiteral("on")) {
              cfg.enabled = true;
            } else if (mode == QStringLiteral("off")) {
              cfg.enabled = false;
            } else if (mode == QStringLiteral("toggle")) {
              cfg.enabled = !cfg.enabled;
            } else {
              err << "headroomctl: patchbay autoconnect enable expects on|off|toggle\n";
              exitCode = 2;
              break;
            }
            saveAutoConnectConfig(s, cfg);
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("enabled"), cfg.enabled);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "enabled\t" << (cfg.enabled ? "on" : "off") << "\n";
            }
            exitCode = 0;
            break;
          }

          if (sub2 == QStringLiteral("apply")) {
            const AutoConnectConfig cfg = loadCfg();
            const AutoConnectApplyResult r = applyAutoConnectRules(graph, cfg);
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), r.errors.isEmpty());
              o.insert(QStringLiteral("createdLinks"), r.linksCreated);
              o.insert(QStringLiteral("alreadyPresentLinks"), r.linksAlreadyPresent);
              o.insert(QStringLiteral("rulesConsidered"), r.rulesConsidered);
              o.insert(QStringLiteral("rulesApplied"), r.rulesApplied);
              QJsonArray errors;
              for (const auto& e : r.errors) {
                errors.append(e);
              }
              o.insert(QStringLiteral("errors"), errors);
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "applied\tcreated=" << r.linksCreated << "\talready=" << r.linksAlreadyPresent << "\trules=" << r.rulesApplied
                  << "\terrors=" << r.errors.size() << "\n";
              for (const auto& e : r.errors) {
                err << "error:\t" << e << "\n";
              }
            }
            exitCode = r.errors.isEmpty() ? 0 : 1;
            break;
          }

          if (sub2 == QStringLiteral("rules")) {
            const AutoConnectConfig cfg = loadCfg();
            QVector<AutoConnectRule> rules = cfg.rules;
            std::sort(rules.begin(), rules.end(), [](const AutoConnectRule& a, const AutoConnectRule& b) { return a.name.toLower() < b.name.toLower(); });
            if (jsonOutput) {
              QJsonArray arr;
              for (const auto& r : rules) {
                QJsonObject o;
                o.insert(QStringLiteral("name"), r.name);
                o.insert(QStringLiteral("enabled"), r.enabled);
                o.insert(QStringLiteral("outputNodeRegex"), r.outputNodeRegex);
                o.insert(QStringLiteral("outputPortRegex"), r.outputPortRegex);
                o.insert(QStringLiteral("inputNodeRegex"), r.inputNodeRegex);
                o.insert(QStringLiteral("inputPortRegex"), r.inputPortRegex);
                arr.append(o);
              }
              out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              for (const auto& r : rules) {
                out << r.name << "\t" << (r.enabled ? "enabled" : "disabled") << "\t" << r.outputNodeRegex << "/" << r.outputPortRegex << " -> "
                    << r.inputNodeRegex << "/" << r.inputPortRegex << "\n";
              }
            }
            exitCode = 0;
            break;
          }

          if (sub2 == QStringLiteral("rule")) {
            if (rest.size() < 2) {
              printUsage(err);
              exitCode = 2;
              break;
            }
            const QString sub3 = rest.value(1).trimmed().toLower();

            if (sub3 == QStringLiteral("add")) {
              const bool disabled = rest.contains(QStringLiteral("--disabled"));
              if (disabled) {
                rest.removeAll(QStringLiteral("--disabled"));
              }
              if (rest.size() < 7) {
                err << "headroomctl: patchbay autoconnect rule add expects <name> <out-node-re> <out-port-re> <in-node-re> <in-port-re>\n";
                exitCode = 2;
                break;
              }
              AutoConnectRule r;
              r.name = rest.value(2).trimmed();
              r.outputNodeRegex = rest.value(3).trimmed();
              r.outputPortRegex = rest.value(4).trimmed();
              r.inputNodeRegex = rest.value(5).trimmed();
              r.inputPortRegex = rest.value(6).trimmed();
              r.enabled = !disabled;
              if (r.name.isEmpty() || r.outputNodeRegex.isEmpty() || r.outputPortRegex.isEmpty() || r.inputNodeRegex.isEmpty() || r.inputPortRegex.isEmpty()) {
                err << "headroomctl: patchbay autoconnect rule add expects non-empty fields\n";
                exitCode = 2;
                break;
              }
              AutoConnectRuleStore::save(s, r);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("name"), r.name);
                o.insert(QStringLiteral("enabled"), r.enabled);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << "saved\t" << r.name << "\t" << (r.enabled ? "enabled" : "disabled") << "\n";
              }
              exitCode = 0;
              break;
            }

            if (sub3 == QStringLiteral("enable")) {
              if (rest.size() < 4) {
                err << "headroomctl: patchbay autoconnect rule enable expects <name> on|off|toggle\n";
                exitCode = 2;
                break;
              }
              const QString name = rest.value(2).trimmed();
              const QString mode = rest.value(3).trimmed().toLower();
              if (name.isEmpty()) {
                err << "headroomctl: patchbay autoconnect rule enable expects <name>\n";
                exitCode = 2;
                break;
              }
              auto rule = AutoConnectRuleStore::load(s, name);
              if (!rule) {
                err << "headroomctl: autoconnect rule not found: " << name << "\n";
                exitCode = 1;
                break;
              }
              if (mode == QStringLiteral("on")) {
                rule->enabled = true;
              } else if (mode == QStringLiteral("off")) {
                rule->enabled = false;
              } else if (mode == QStringLiteral("toggle")) {
                rule->enabled = !rule->enabled;
              } else {
                err << "headroomctl: patchbay autoconnect rule enable expects on|off|toggle\n";
                exitCode = 2;
                break;
              }
              AutoConnectRuleStore::save(s, *rule);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), true);
                o.insert(QStringLiteral("name"), rule->name);
                o.insert(QStringLiteral("enabled"), rule->enabled);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << "enabled\t" << rule->name << "\t" << (rule->enabled ? "on" : "off") << "\n";
              }
              exitCode = 0;
              break;
            }

            if (sub3 == QStringLiteral("delete")) {
              if (rest.size() < 3) {
                err << "headroomctl: patchbay autoconnect rule delete expects <name>\n";
                exitCode = 2;
                break;
              }
              const QString name = rest.value(2).trimmed();
              if (name.isEmpty()) {
                err << "headroomctl: patchbay autoconnect rule delete expects <name>\n";
                exitCode = 2;
                break;
              }
              const bool removed = AutoConnectRuleStore::remove(s, name);
              if (jsonOutput) {
                QJsonObject o;
                o.insert(QStringLiteral("ok"), removed);
                o.insert(QStringLiteral("name"), name);
                out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                out << (removed ? "deleted" : "not-found") << "\t" << name << "\n";
              }
              exitCode = removed ? 0 : 1;
              break;
            }

            printUsage(err);
            exitCode = 2;
            break;
          }

          auto handleListMutate = [&](const QString& which, bool isWhitelist) {
            AutoConnectConfig cfg = loadCfg();
            QStringList& list = isWhitelist ? cfg.whitelist : cfg.blacklist;

            if (rest.size() < 2) {
              printUsage(err);
              exitCode = 2;
              return;
            }
            const QString op = rest.value(1).trimmed().toLower();
            if (op == QStringLiteral("list")) {
              if (jsonOutput) {
                QJsonArray arr;
                for (const auto& v : list) {
                  arr.append(v);
                }
                out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
              } else {
                for (const auto& v : list) {
                  out << v << "\n";
                }
              }
              exitCode = 0;
              return;
            }

            if (op == QStringLiteral("add")) {
              if (rest.size() < 3) {
                err << "headroomctl: patchbay autoconnect " << which << " add expects <regex>\n";
                exitCode = 2;
                return;
              }
              const QString value = rest.value(2).trimmed();
              if (!value.isEmpty()) {
                list.push_back(value);
              }
              saveAutoConnectConfig(s, cfg);
              exitCode = 0;
              return;
            }

            if (op == QStringLiteral("remove")) {
              if (rest.size() < 3) {
                err << "headroomctl: patchbay autoconnect " << which << " remove expects <regex>\n";
                exitCode = 2;
                return;
              }
              const QString value = rest.value(2).trimmed();
              list.removeAll(value);
              saveAutoConnectConfig(s, cfg);
              exitCode = 0;
              return;
            }

            printUsage(err);
            exitCode = 2;
          };

          if (sub2 == QStringLiteral("whitelist")) {
            handleListMutate(QStringLiteral("whitelist"), true);
            break;
          }
          if (sub2 == QStringLiteral("blacklist")) {
            handleListMutate(QStringLiteral("blacklist"), false);
            break;
          }

          printUsage(err);
          exitCode = 2;
          break;
        }

        printUsage(err);
        exitCode = 2;
        break;
      }

      if (cmd == QStringLiteral("session") || cmd == QStringLiteral("sessions")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        QStringList rest = args.mid(2);
        const QString sub = rest.value(0).trimmed().toLower();

        const bool strictLinks = rest.contains(QStringLiteral("--strict-links"));
        if (strictLinks) {
          rest.removeAll(QStringLiteral("--strict-links"));
        }

        const bool mergeSettings = rest.contains(QStringLiteral("--merge-settings"));
        if (mergeSettings) {
          rest.removeAll(QStringLiteral("--merge-settings"));
        }

        const bool strictSettings = !mergeSettings;

        QSettings s;

        if (sub == QStringLiteral("list")) {
          const QStringList names = SessionSnapshotStore::listSnapshotNames(s);
          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& name : names) {
              arr.append(name);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& name : names) {
              out << name << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("save")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session save expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const SessionSnapshot snap = snapshotSession(name, graph, s);
          SessionSnapshotStore::save(s, snap);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("name"), snap.name);
            o.insert(QStringLiteral("links"), static_cast<int>(snap.links.size()));
            o.insert(QStringLiteral("eqNodes"), static_cast<int>(snap.eqByNodeName.size()));
            o.insert(QStringLiteral("positions"), static_cast<int>(snap.patchbayPositionByNodeName.size()));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "saved\t" << snap.name << "\tlinks=" << snap.links.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("apply")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }

          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session apply expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const auto snap = SessionSnapshotStore::load(s, name);
          if (!snap) {
            err << "headroomctl: session snapshot not found: " << name << "\n";
            exitCode = 1;
            break;
          }

          const SessionSnapshotApplyResult r = applySessionSnapshot(graph, s, *snap, strictLinks, strictSettings);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), r.errors.isEmpty());
            o.insert(QStringLiteral("name"), snap->name);
            o.insert(QStringLiteral("strictLinks"), strictLinks);
            o.insert(QStringLiteral("strictSettings"), strictSettings);
            QJsonObject patchbay;
            patchbay.insert(QStringLiteral("strict"), strictLinks);
            patchbay.insert(QStringLiteral("desiredLinks"), r.patchbay.desiredLinks);
            patchbay.insert(QStringLiteral("createdLinks"), r.patchbay.createdLinks);
            patchbay.insert(QStringLiteral("alreadyPresentLinks"), r.patchbay.alreadyPresentLinks);
            patchbay.insert(QStringLiteral("disconnectedLinks"), r.patchbay.disconnectedLinks);
            patchbay.insert(QStringLiteral("missingEndpoints"), r.patchbay.missingEndpoints);
            o.insert(QStringLiteral("patchbay"), patchbay);
            o.insert(QStringLiteral("defaultSinkRequested"), r.defaultSinkRequested);
            o.insert(QStringLiteral("defaultSinkSet"), r.defaultSinkSet);
            o.insert(QStringLiteral("defaultSourceRequested"), r.defaultSourceRequested);
            o.insert(QStringLiteral("defaultSourceSet"), r.defaultSourceSet);
            QJsonArray missing;
            for (const auto& m : r.missing) {
              missing.append(m);
            }
            QJsonArray errors;
            for (const auto& e : r.errors) {
              errors.append(e);
            }
            o.insert(QStringLiteral("missing"), missing);
            o.insert(QStringLiteral("errors"), errors);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "applied\t" << snap->name << "\tlinks(created=" << r.patchbay.createdLinks << ", already=" << r.patchbay.alreadyPresentLinks
                << ", missing=" << r.patchbay.missingEndpoints << ", errors=" << r.patchbay.errors.size() << ")\n";
            for (const auto& m : r.missing) {
              err << "missing:\t" << m << "\n";
            }
            for (const auto& e : r.errors) {
              err << "error:\t" << e << "\n";
            }
          }
          exitCode = r.errors.isEmpty() ? 0 : 1;
          break;
        }

        if (sub == QStringLiteral("delete")) {
          if (rest.size() < 2) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString name = rest.value(1).trimmed();
          if (name.isEmpty()) {
            err << "headroomctl: session delete expects <snapshot-name>\n";
            exitCode = 2;
            break;
          }

          const bool removed = SessionSnapshotStore::remove(s, name);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), removed);
            o.insert(QStringLiteral("name"), name);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << (removed ? "deleted" : "not-found") << "\t" << name << "\n";
          }
          exitCode = removed ? 0 : 1;
          break;
        }

        printUsage(err);
        exitCode = 2;
        break;
      }

      if (cmd == QStringLiteral("eq")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        if (sub == QStringLiteral("presets")) {
          const auto presets = builtinEqPresets();
          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& p : presets) {
              arr.append(p.first);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& p : presets) {
              out << p.first << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("list")) {
          QList<PwNodeInfo> targets = graph.audioSinks();
          targets.append(graph.audioSources());

          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& n : targets) {
              const EqPreset preset = loadEqPresetForNodeName(n.name);
              QJsonObject o = nodeToJson(n);
              o.insert(QStringLiteral("preset"), eqPresetToJson(preset));
              arr.append(o);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            for (const auto& n : targets) {
              const EqPreset preset = loadEqPresetForNodeName(n.name);
              out << n.id << "\t" << n.mediaClass << "\t" << nodeLabel(n) << "\t" << (preset.enabled ? "on" : "off") << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("get")) {
          if (args.size() < 4) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          if (nodeName.isEmpty()) {
            err << "headroomctl: eq get expects <node-id|node-name>\n";
            exitCode = 2;
            break;
          }

          const EqPreset preset = loadEqPresetForNodeName(nodeName);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("preset"), eqPresetToJson(preset));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\t" << (preset.enabled ? "on" : "off") << "\tpreampDb=" << preset.preampDb << "\tbands=" << preset.bands.size() << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("enable")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          const QString mode = args.at(4).trimmed().toLower();
          if (nodeName.isEmpty()) {
            err << "headroomctl: eq enable expects <node-id|node-name> on|off|toggle\n";
            exitCode = 2;
            break;
          }

          EqPreset preset = loadEqPresetForNodeName(nodeName);
          bool enabled = preset.enabled;
          if (mode == QStringLiteral("on") || mode == QStringLiteral("true") || mode == QStringLiteral("1")) {
            enabled = true;
          } else if (mode == QStringLiteral("off") || mode == QStringLiteral("false") || mode == QStringLiteral("0")) {
            enabled = false;
          } else if (mode == QStringLiteral("toggle")) {
            enabled = !enabled;
          } else {
            err << "headroomctl: eq enable expects on|off|toggle\n";
            exitCode = 2;
            break;
          }

          preset.enabled = enabled;
          saveEqPresetForNodeName(nodeName, preset);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("enabled"), enabled);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\t" << (enabled ? "on" : "off") << "\n";
          }
          exitCode = 0;
          break;
        }

        if (sub == QStringLiteral("preset")) {
          if (args.size() < 5) {
            printUsage(err);
            exitCode = 2;
            break;
          }
          const QString nodeName = resolveNodeName(args.at(3), &graph);
          const QString presetName = args.mid(4).join(' ');
          if (nodeName.isEmpty() || presetName.trimmed().isEmpty()) {
            err << "headroomctl: eq preset expects <node-id|node-name> <preset-name>\n";
            exitCode = 2;
            break;
          }

          auto presetOpt = builtinEqPresetByName(presetName);
          if (!presetOpt) {
            err << "headroomctl: unknown preset. Try: headroomctl eq presets\n";
            exitCode = 2;
            break;
          }

          EqPreset preset = *presetOpt;
          preset.enabled = true;
          saveEqPresetForNodeName(nodeName, preset);

          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("nodeName"), nodeName);
            o.insert(QStringLiteral("presetName"), presetName);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << nodeName << "\tapplied\t" << presetName << "\n";
          }
          exitCode = 0;
          break;
        }

        err << "headroomctl: unknown eq subcommand\n";
        exitCode = 2;
        break;
      }

      if (cmd == QStringLiteral("set-volume")) {
        if (args.size() < 4) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const auto id = parseNodeId(args.at(2));
        const auto volume = parseVolumeValue(args.at(3));
        if (!id || !volume) {
          err << "headroomctl: invalid node-id or volume value\n";
          exitCode = 2;
          break;
        }

        // Give the PipeWire registry a moment to populate; for one-shot commands,
        // graph discovery can race the initial setNodeVolume call.
        bool ok = false;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
          ok = graph.setNodeVolume(*id, *volume);
          if (ok) {
            break;
          }
          waitForGraph(60);
        }
        if (!ok) {
          err << "headroomctl: failed to set volume\n";
          exitCode = 1;
          break;
        }

        // Give PipeWire a short moment to flush the request before exiting.
        waitForGraph(120);

        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("mute")) {
        if (args.size() < 4) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const auto id = parseNodeId(args.at(2));
        if (!id) {
          err << "headroomctl: invalid node-id\n";
          exitCode = 2;
          break;
        }

        // Same rationale as set-volume: make single-shot mute reliable even when
        // the PipeWire graph is still being discovered.
        waitForGraph(120);

        const QString mode = args.at(3).trimmed().toLower();
        const PwNodeControls c = graph.nodeControls(*id).value_or(PwNodeControls{});

        bool target = c.mute;
        if (mode == QStringLiteral("on")) {
          target = true;
        } else if (mode == QStringLiteral("off")) {
          target = false;
        } else if (mode == QStringLiteral("toggle")) {
          target = !c.mute;
        } else {
          err << "headroomctl: mute expects on|off|toggle\n";
          exitCode = 2;
          break;
        }

        bool ok = false;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500) {
          ok = graph.setNodeMute(*id, target);
          if (ok) {
            break;
          }
          waitForGraph(60);
        }
        if (!ok) {
          err << "headroomctl: failed to set mute\n";
          exitCode = 1;
          break;
        }

        // Give PipeWire a short moment to flush the request before exiting.
        waitForGraph(120);

        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("connect")) {
        const bool force = args.contains(QStringLiteral("--force"));
        if (force) {
          args.removeAll(QStringLiteral("--force"));
        }

        if (args.size() < 4) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const auto outPortId = parseNodeId(args.at(2));
        const auto inPortId = parseNodeId(args.at(3));
        if (!outPortId || !inPortId) {
          err << "headroomctl: connect expects <out-port-id> <in-port-id>\n";
          exitCode = 2;
          break;
        }

        const QList<PwPortInfo> ports = graph.ports();
        const auto outPort = portById(ports, *outPortId);
        const auto inPort = portById(ports, *inPortId);
        if (!outPort || !inPort) {
          err << "headroomctl: unknown port id\n";
          exitCode = 2;
          break;
        }
        if (outPort->direction != QStringLiteral("out") || inPort->direction != QStringLiteral("in")) {
          err << "headroomctl: connect expects an output port then an input port\n";
          exitCode = 2;
          break;
        }

        if (!force) {
          QSettings s;
          const PwNodeInfo outNode = graph.nodeById(outPort->nodeId).value_or(PwNodeInfo{});
          const PwNodeInfo inNode = graph.nodeById(inPort->nodeId).value_or(PwNodeInfo{});
          if (PatchbayPortConfigStore::isLocked(s, outNode.name, outPort->name) || PatchbayPortConfigStore::isLocked(s, inNode.name, inPort->name)) {
            err << "headroomctl: connect blocked (port locked). Use --force to override.\n";
            exitCode = 1;
            break;
          }
        }

        const auto existing = linkByPorts(graph.links(), *outPortId, *inPortId);
        if (existing) {
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("alreadyExists"), true);
            o.insert(QStringLiteral("linkId"), static_cast<qint64>(existing->id));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "already-connected\t" << existing->id << "\n";
          }
          exitCode = 0;
          break;
        }

        const bool ok = graph.createLink(outPort->nodeId, outPort->id, inPort->nodeId, inPort->id);
        if (!ok) {
          err << "headroomctl: connect failed\n";
          exitCode = 1;
          break;
        }

        std::optional<PwLinkInfo> created;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 750) {
          QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
          created = linkByPorts(graph.links(), *outPortId, *inPortId);
          if (created) {
            break;
          }
          QThread::msleep(10);
        }

        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("alreadyExists"), false);
          if (created) {
            o.insert(QStringLiteral("linkId"), static_cast<qint64>(created->id));
          } else {
            o.insert(QStringLiteral("linkId"), QJsonValue::Null);
          }
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else if (created) {
          out << "connected\t" << created->id << "\n";
        } else {
          out << "connected\t" << "unknown-link-id\n";
        }

        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("disconnect") || cmd == QStringLiteral("unlink")) {
        const bool force = args.contains(QStringLiteral("--force"));
        if (force) {
          args.removeAll(QStringLiteral("--force"));
        }

        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        std::optional<uint32_t> linkId;
        if (args.size() == 3) {
          linkId = parseNodeId(args.at(2));
          if (!linkId) {
            err << "headroomctl: disconnect expects <link-id> or <out-port-id> <in-port-id>\n";
            exitCode = 2;
            break;
          }
        } else {
          const auto outPortId = parseNodeId(args.at(2));
          const auto inPortId = parseNodeId(args.at(3));
          if (!outPortId || !inPortId) {
            err << "headroomctl: disconnect expects <link-id> or <out-port-id> <in-port-id>\n";
            exitCode = 2;
            break;
          }
          if (const auto l = linkByPorts(graph.links(), *outPortId, *inPortId)) {
            linkId = l->id;
          }
          if (!linkId) {
            err << "headroomctl: no such link\n";
            exitCode = 1;
            break;
          }
        }

        if (!force) {
          std::optional<PwLinkInfo> l;
          for (const auto& candidate : graph.links()) {
            if (candidate.id == *linkId) {
              l = candidate;
              break;
            }
          }
          if (l) {
            const auto outPort = portById(graph.ports(), l->outputPortId);
            const auto inPort = portById(graph.ports(), l->inputPortId);
            const PwNodeInfo outNode = graph.nodeById(l->outputNodeId).value_or(PwNodeInfo{});
            const PwNodeInfo inNode = graph.nodeById(l->inputNodeId).value_or(PwNodeInfo{});
            QSettings s;
            const QString outPortName = outPort ? outPort->name : QString{};
            const QString inPortName = inPort ? inPort->name : QString{};
            if (PatchbayPortConfigStore::isLocked(s, outNode.name, outPortName) || PatchbayPortConfigStore::isLocked(s, inNode.name, inPortName)) {
              err << "headroomctl: disconnect blocked (port locked). Use --force to override.\n";
              exitCode = 1;
              break;
            }
          }
        }

        const bool ok = graph.destroyLink(*linkId);
        if (!ok) {
          err << "headroomctl: disconnect failed\n";
          exitCode = 1;
          break;
        }

        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), true);
          o.insert(QStringLiteral("linkId"), static_cast<qint64>(*linkId));
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << "disconnected\t" << *linkId << "\n";
        }

        exitCode = 0;
        break;
      }

      if (cmd == QStringLiteral("record")) {
        if (args.size() < 3) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString sub = args.at(2).trimmed().toLower();
        if (sub == QStringLiteral("targets")) {
          auto streamLabel = [&](const PwNodeInfo& n) -> QString {
            const QString base = nodeLabel(n);
            const QString app = !n.appName.isEmpty() ? n.appName : n.appProcessBinary;
            return (!app.isEmpty() && app != base) ? QStringLiteral("%1 — %2").arg(app, base) : base;
          };

          struct Target final {
            QString kind;
            QString label;
            std::optional<uint32_t> nodeId;
            QString targetObject;
            bool captureSink = true;
          };

          QVector<Target> targets;
          targets.push_back({QStringLiteral("system-mix"), QStringLiteral("System mix (default output monitor)"), std::nullopt, QString{}, true});

          for (const auto& s : graph.audioSinks()) {
            targets.push_back({QStringLiteral("output-device"), QStringLiteral("Output: %1").arg(nodeLabel(s)), s.id, s.name, true});
          }
          for (const auto& s : graph.audioPlaybackStreams()) {
            targets.push_back({QStringLiteral("app-playback"), QStringLiteral("App playback: %1").arg(streamLabel(s)), s.id, s.name, false});
          }

          targets.push_back({QStringLiteral("default-input"), QStringLiteral("Default input (mic)"), std::nullopt, QString{}, false});

          for (const auto& s : graph.audioSources()) {
            targets.push_back({QStringLiteral("input-device"), QStringLiteral("Input: %1").arg(nodeLabel(s)), s.id, s.name, false});
          }
          for (const auto& s : graph.audioCaptureStreams()) {
            targets.push_back({QStringLiteral("app-recording"), QStringLiteral("App recording: %1").arg(streamLabel(s)), s.id, s.name, true});
          }

          if (jsonOutput) {
            QJsonArray arr;
            for (const auto& t : targets) {
              QJsonObject o;
              o.insert(QStringLiteral("kind"), t.kind);
              o.insert(QStringLiteral("label"), t.label);
              o.insert(QStringLiteral("targetObject"), t.targetObject);
              o.insert(QStringLiteral("captureSink"), t.captureSink);
              if (t.nodeId) {
                o.insert(QStringLiteral("nodeId"), static_cast<qint64>(*t.nodeId));
              }
              arr.append(o);
            }
            out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "kind\tcapture\tid\ttargetObject\tlabel\n";
            for (const auto& t : targets) {
              out << t.kind << "\t" << (t.captureSink ? "sink" : "source") << "\t" << (t.nodeId ? QString::number(*t.nodeId) : "-")
                  << "\t" << (t.targetObject.isEmpty() ? "-" : t.targetObject) << "\t" << t.label << "\n";
            }
          }

          exitCode = 0;
          break;
        }

        if (args.size() < 4) {
          printUsage(err);
          exitCode = 2;
          break;
        }

        const QString filePath = args.at(3).trimmed();

        QString targetArg;
        std::optional<AudioRecorder::Format> formatOpt;
        int durationSec = 0;
        bool captureSink = true;
        bool background = false;

        for (int i = 4; i < args.size(); ++i) {
          const QString a = args.at(i).trimmed();
          if (a == QStringLiteral("--target")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --target requires a value\n";
              exitCode = 2;
              break;
            }
            targetArg = args.at(i + 1);
            ++i;
            continue;
          }
          if (a == QStringLiteral("--format")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --format requires a value\n";
              exitCode = 2;
              break;
            }
            const auto f = AudioRecorder::formatFromString(args.at(i + 1));
            if (!f) {
              err << "headroomctl: invalid --format (expected wav|flac)\n";
              exitCode = 2;
              break;
            }
            formatOpt = *f;
            ++i;
            continue;
          }
          if (a == QStringLiteral("--duration") || a == QStringLiteral("--seconds")) {
            if (i + 1 >= args.size()) {
              err << "headroomctl: --duration requires a value\n";
              exitCode = 2;
              break;
            }
            bool ok = false;
            const int v = args.at(i + 1).trimmed().toInt(&ok);
            if (!ok || v < 0) {
              err << "headroomctl: invalid --duration (expected seconds >= 0)\n";
              exitCode = 2;
              break;
            }
            durationSec = v;
            ++i;
            continue;
          }
          if (a == QStringLiteral("--sink")) {
            captureSink = true;
            continue;
          }
          if (a == QStringLiteral("--source")) {
            captureSink = false;
            continue;
          }
          if (a == QStringLiteral("--background") || a == QStringLiteral("--daemon")) {
            background = true;
            continue;
          }
          err << "headroomctl: unknown record option: " << a << "\n";
          exitCode = 2;
          break;
        }
        if (exitCode != 0) {
          break;
        }

        auto inferFormatFromPath = [&](const QString& p) -> AudioRecorder::Format {
          const QString lower = p.trimmed().toLower();
          if (lower.endsWith(QStringLiteral(".flac"))) {
            return AudioRecorder::Format::Flac;
          }
          return AudioRecorder::Format::Wav;
        };
        const AudioRecorder::Format format = formatOpt.value_or(inferFormatFromPath(filePath));

        if (sub == QStringLiteral("start")) {
          if (background) {
            QStringList daemonArgs;
            daemonArgs << QStringLiteral("record") << QStringLiteral("run") << filePath;
            if (!targetArg.trimmed().isEmpty()) {
              daemonArgs << QStringLiteral("--target") << targetArg;
            }
            daemonArgs << QStringLiteral("--format") << AudioRecorder::formatToString(format);
            daemonArgs << QStringLiteral("--duration") << QString::number(durationSec);
            daemonArgs << (captureSink ? QStringLiteral("--sink") : QStringLiteral("--source"));

            qint64 pid = 0;
            const bool ok = QProcess::startDetached(QCoreApplication::applicationFilePath(), daemonArgs, QString{}, &pid);
            if (!ok) {
              err << "headroomctl: failed to start detached recorder\n";
              exitCode = 1;
              break;
            }
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), true);
              o.insert(QStringLiteral("pid"), pid);
              o.insert(QStringLiteral("statusPath"), recordingStatusPath());
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "started\tpid=" << pid << "\n";
            }
            exitCode = 0;
            break;
          }

          // Foreground start = run.
        }

        if (sub != QStringLiteral("run") && sub != QStringLiteral("start")) {
          err << "headroomctl: record expects start|stop|status\n";
          exitCode = 2;
          break;
        }

        bool okId = false;
        const uint32_t nodeId = targetArg.trimmed().toUInt(&okId);
        QString targetObject = targetArg.trimmed();
        if (okId) {
          const auto n = graph.nodeById(nodeId);
          if (!n) {
            err << "headroomctl: unknown node id " << nodeId << "\n";
            exitCode = 2;
            break;
          }
          targetObject = n->name;
        }

        const QString statusPath = recordingStatusPath();
        if (const auto existing = readJsonObjectFile(statusPath, nullptr)) {
          const qint64 pid = existing->value(QStringLiteral("pid")).toVariant().toLongLong();
          const bool running = pidAlive(pid) && existing->value(QStringLiteral("running")).toBool(true);
          if (running) {
            err << "headroomctl: already recording (pid " << pid << "). Stop first.\n";
            exitCode = 1;
            break;
          }
        }

        g_recordStopRequested.store(false);
        std::signal(SIGINT, onRecordSignal);
        std::signal(SIGTERM, onRecordSignal);

        QString targetLabel;
        if (targetObject.isEmpty()) {
          targetLabel = captureSink ? QStringLiteral("system-mix") : QStringLiteral("default-input");
        } else {
          targetLabel = targetObject;
          for (const auto& n : graph.nodes()) {
            if (n.name == targetObject) {
              targetLabel = nodeLabel(n);
              break;
            }
          }
        }

        const QString resolvedPath = AudioRecorder::expandPathTemplate(filePath, targetLabel, format);

        AudioRecorder recorder(&pw);
        AudioRecorder::StartOptions recOpts;
        recOpts.filePath = resolvedPath;
        recOpts.targetObject = targetObject;
        recOpts.captureSink = captureSink;
        recOpts.format = format;
        if (!recorder.start(recOpts)) {
          const QString e = recorder.lastError();
          err << "headroomctl: record start failed" << (e.isEmpty() ? "" : ": ") << e << "\n";
          exitCode = 1;
          break;
        }

        const qint64 pid = QCoreApplication::applicationPid();
        const QString startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        QJsonObject state;
        state.insert(QStringLiteral("pid"), pid);
        state.insert(QStringLiteral("running"), true);
        state.insert(QStringLiteral("startedAt"), startedAt);
        state.insert(QStringLiteral("filePathTemplate"), filePath);
        state.insert(QStringLiteral("filePath"), recorder.filePath());
        state.insert(QStringLiteral("format"), AudioRecorder::formatToString(format));
        state.insert(QStringLiteral("targetObject"), targetObject);
        state.insert(QStringLiteral("targetLabel"), targetLabel);
        state.insert(QStringLiteral("captureSink"), captureSink);
        if (durationSec > 0) {
          state.insert(QStringLiteral("durationLimitSeconds"), durationSec);
        }

        if (!graph.nodes().isEmpty()) {
          auto snapshotList = [&](const QList<PwNodeInfo>& list) {
            QJsonArray arr;
            for (const auto& n : list) {
              arr.append(nodeToJson(n));
            }
            return arr;
          };

          QJsonObject snapshot;
          snapshot.insert(QStringLiteral("sinks"), snapshotList(graph.audioSinks()));
          snapshot.insert(QStringLiteral("sources"), snapshotList(graph.audioSources()));
          snapshot.insert(QStringLiteral("playbackStreams"), snapshotList(graph.audioPlaybackStreams()));
          snapshot.insert(QStringLiteral("captureStreams"), snapshotList(graph.audioCaptureStreams()));

          if (const auto defSink = graph.defaultAudioSinkId()) {
            snapshot.insert(QStringLiteral("defaultAudioSinkId"), static_cast<qint64>(*defSink));
          }
          if (const auto defSource = graph.defaultAudioSourceId()) {
            snapshot.insert(QStringLiteral("defaultAudioSourceId"), static_cast<qint64>(*defSource));
          }

          if (!targetObject.isEmpty()) {
            for (const auto& n : graph.nodes()) {
              if (n.name == targetObject) {
                snapshot.insert(QStringLiteral("targetNodeId"), static_cast<qint64>(n.id));
                snapshot.insert(QStringLiteral("targetMediaClass"), n.mediaClass);
                break;
              }
            }
          }

          state.insert(QStringLiteral("graphSnapshot"), snapshot);
        }

        auto writeStatus = [&]() {
          const uint32_t sr = recorder.sampleRate();
          const uint64_t frames = recorder.framesCaptured();
          const double dur = (sr > 0) ? (static_cast<double>(frames) / static_cast<double>(sr)) : 0.0;

          state.insert(QStringLiteral("sampleRate"), static_cast<int>(sr));
          state.insert(QStringLiteral("channels"), static_cast<int>(recorder.channels()));
          state.insert(QStringLiteral("quantumFrames"), static_cast<int>(recorder.quantumFrames()));
          state.insert(QStringLiteral("lastBufferFrames"), static_cast<int>(recorder.lastBufferFrames()));
          state.insert(QStringLiteral("lastBufferBytes"), static_cast<int>(recorder.lastBufferBytes()));
          state.insert(QStringLiteral("bytesWritten"), static_cast<qint64>(recorder.dataBytesWritten()));
          state.insert(QStringLiteral("framesCaptured"), static_cast<qint64>(frames));
          state.insert(QStringLiteral("durationSeconds"), dur);
          state.insert(QStringLiteral("peakDb"), recorder.peakDb());
          state.insert(QStringLiteral("rmsDb"), recorder.rmsDb());
          state.insert(QStringLiteral("streamState"), recorder.streamStateString());
          const QString le = recorder.lastError();
          if (!le.isEmpty()) {
            state.insert(QStringLiteral("lastError"), le);
          }

          QString writeErr;
          (void)writeJsonFileAtomic(statusPath, state, &writeErr);
        };

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &recorder, [&]() {
          recorder.stop();
          state.insert(QStringLiteral("running"), false);
          state.insert(QStringLiteral("stoppedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
          writeStatus();
        });

        QTimer statusTimer;
        statusTimer.setInterval(500);
        QObject::connect(&statusTimer, &QTimer::timeout, &app, [&]() { writeStatus(); });
        statusTimer.start();

        QTimer stopTimer;
        stopTimer.setInterval(100);
        QObject::connect(&stopTimer, &QTimer::timeout, &app, [&]() {
          if (g_recordStopRequested.load()) {
            app.quit();
          }
        });
        stopTimer.start();

        QTimer durationTimer;
        if (durationSec > 0) {
          durationTimer.setSingleShot(true);
          durationTimer.setInterval(durationSec * 1000);
          QObject::connect(&durationTimer, &QTimer::timeout, &app, [&]() { app.quit(); });
          durationTimer.start();
        }

        writeStatus();

        if (sub == QStringLiteral("start")) {
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("pid"), pid);
            o.insert(QStringLiteral("statusPath"), statusPath);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "recording\tpid=" << pid << "\t(ctrl-c to stop; or headroomctl record stop)\n";
          }
        }

        exitCode = app.exec();
        break;
      }

      printUsage(err);
      exitCode = 2;
    } while (false);
  }

  pw_deinit();
  return exitCode;
}
