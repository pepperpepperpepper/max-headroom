#pragma once

#include "backend/EqConfig.h"
#include "backend/PipeWireGraph.h"

#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <optional>

class QCoreApplication;

namespace headroomctl {
void printUsage(QTextStream& out);
void waitForGraph(int waitMs);

std::optional<uint32_t> parseNodeId(const QString& s);
std::optional<float> parseVolumeValue(const QString& input);

QString nodeLabel(const PwNodeInfo& n);

QString runtimeDirPath();
QString recordingStatusPath();

bool writeJsonFileAtomic(const QString& path, const QJsonObject& obj, QString* errorOut);
std::optional<QJsonObject> readJsonObjectFile(const QString& path, QString* errorOut);

bool pidAlive(qint64 pid);
std::optional<bool> processRunningExact(const QString& name);
bool stopPid(qint64 pid, int timeoutMs, QTextStream& err);

QString eqPresetKeyForNodeName(const QString& nodeName);
EqPreset loadEqPresetForNodeName(const QString& nodeName);
void saveEqPresetForNodeName(const QString& nodeName, const EqPreset& preset);

QString normalizePresetName(const QString& in);
QVector<QPair<QString, EqPreset>> builtinEqPresets();
std::optional<EqPreset> builtinEqPresetByName(const QString& presetName);

QString resolveNodeName(const QString& nodeIdOrName, PipeWireGraph* graph);

void printNodes(QTextStream& out, const QList<PwNodeInfo>& nodes, PipeWireGraph& graph);
void printAllNodes(QTextStream& out, const QList<PwNodeInfo>& nodes);

std::optional<PwPortInfo> portById(const QList<PwPortInfo>& ports, uint32_t portId);
std::optional<PwLinkInfo> linkByPorts(const QList<PwLinkInfo>& links, uint32_t outPortId, uint32_t inPortId);

QJsonObject nodeToJson(const PwNodeInfo& n);
QJsonObject nodeControlsToJson(const PwNodeControls& c);
QJsonObject portToJson(const PwPortInfo& p, const std::optional<PwNodeInfo>& node);
QJsonObject linkToJson(const PwLinkInfo& l,
                       const std::optional<PwNodeInfo>& outNode,
                       const std::optional<PwPortInfo>& outPort,
                       const std::optional<PwNodeInfo>& inNode,
                       const std::optional<PwPortInfo>& inPort);

int runCommand(QCoreApplication& app, QStringList args, QTextStream& out, QTextStream& err);
} // namespace headroomctl
