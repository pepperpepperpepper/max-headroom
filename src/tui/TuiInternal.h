#pragma once

#include "backend/AudioRecorder.h"
#include "backend/EngineControl.h"
#include "backend/EqConfig.h"
#include "backend/PipeWireGraph.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class QCoreApplication;
class EqManager;

namespace headroomtui {
inline constexpr float kMinVolume = 0.0f;
inline constexpr float kMaxVolume = 2.0f;
inline constexpr float kVolumeStep = 0.05f;
inline constexpr int kMainLoopTimeoutMs = 80;

void printUsage();

enum class Page : int {
  Outputs = 0,
  Inputs = 1,
  Streams = 2,
  Patchbay = 3,
  Eq = 4,
  Recording = 5,
  Status = 6,
  Engine = 7,
};

Page pageFromIndex(int idx);
int clampIndex(int idx, int size);
float clampVolume(float v);

QString displayNameForNode(const PwNodeInfo& n);
QStringList defaultSinksOrder(const QList<PwNodeInfo>& sinks);
QList<PwNodeInfo> applySinksOrder(const QList<PwNodeInfo>& sinks, QSettings& s);
QList<PwNodeInfo> eqTargetsForGraph(PipeWireGraph* graph);
bool moveSinkInOrder(const QList<PwNodeInfo>& sinks, const QString& sinkName, int delta, QString* statusOut);

enum class PortKind {
  Audio,
  Midi,
  Other,
};

PortKind portKindFor(const PwPortInfo& p, const QHash<uint32_t, PwNodeInfo>& nodesById);
const char* portKindLabelShort(PortKind k);

QVector<QPair<QString, EqPreset>> builtinEqPresets();
int promptSelectPresetIndex(const QVector<QPair<QString, EqPreset>>& presets, int currentIndex, int height, int width);

void promptScrollLines(const char* title, const QStringList& lines, int height, int width);
QString promptInputLine(const char* title, const char* prompt, const QString& currentValue, int height, int width);

struct RecordingTarget final {
  QString label;
  QString targetObject;
  bool captureSink = true;
};

struct RecordingGraphSnapshot final {
  QString capturedAtUtc;
  QList<PwNodeInfo> sinks;
  QList<PwNodeInfo> sources;
  QList<PwNodeInfo> playbackStreams;
  QList<PwNodeInfo> captureStreams;
  std::optional<uint32_t> defaultSinkId;
  std::optional<uint32_t> defaultSourceId;
};

RecordingGraphSnapshot captureRecordingSnapshot(PipeWireGraph* graph);
QStringList recordingSnapshotLines(const RecordingGraphSnapshot& snap);
QVector<RecordingTarget> buildRecordingTargets(PipeWireGraph* graph);

struct StreamRoute final {
  uint32_t deviceId = 0;
  QString deviceName;
  bool isPlayback = true;
};

StreamRoute routeForStream(PipeWireGraph* graph, const PwNodeInfo& stream);
bool movePlaybackStreamToSink(PipeWireGraph* graph, uint32_t streamId, uint32_t sinkId);
bool moveCaptureStreamToSource(PipeWireGraph* graph, uint32_t streamId, uint32_t sourceId);

QList<PwPortInfo> portsForNode(const QList<PwPortInfo>& ports, uint32_t nodeId, const QString& direction);
QList<PwNodeInfo> nodesWithPortDirection(const QList<PwNodeInfo>& nodes, const QList<PwPortInfo>& ports, const QString& direction);

QString nodeSummary(const QString& prefix, const PwNodeInfo& n, const PwNodeControls& c, bool showName);

uint32_t promptSelectPortId(const char* title,
                            const QList<PwPortInfo>& ports,
                            uint32_t currentId,
                            int height,
                            int width,
                            const QHash<uint32_t, PwNodeInfo>& nodesById);
uint32_t promptSelectNodeId(const char* title, const QList<PwNodeInfo>& nodes, uint32_t currentId, int height, int width);

void drawHeader(Page page, int width);
void drawStatusBar(const QString& text, int height, int width);
void drawHelpOverlay(Page page, int height, int width);
void drawListPage(const char* title,
                  const QList<PwNodeInfo>& devices,
                  PipeWireGraph* graph,
                  int& selectedIdx,
                  std::optional<uint32_t> defaultNodeId,
                  int height,
                  int width);
void drawEqPage(PipeWireGraph* graph, EqManager* eq, int& selectedIdx, const QString& statusLine, int height, int width);
void drawRecordingPage(PipeWireGraph* graph,
                       AudioRecorder* recorder,
                       const RecordingGraphSnapshot* snapshot,
                       int& selectedTargetIdx,
                       const QString& filePathOrTemplate,
                       AudioRecorder::Format selectedFormat,
                       int durationLimitSec,
                       const QString& statusLine,
                       int height,
                       int width);
void drawStreamsPage(PipeWireGraph* graph, int& selectedIdx, int height, int width);
void drawPatchbayPage(PipeWireGraph* graph, int& selectedLinkIdx, const QString& statusLine, int height, int width);
void drawStatusPage(PipeWireGraph* graph, int& selectedIdx, int height, int width);
void drawEnginePage(const QList<SystemdUnitStatus>& units, int& selectedIdx, const QString& engineStatus, int height, int width);

int runTui(QCoreApplication& app, QStringList args);
} // namespace headroomtui

