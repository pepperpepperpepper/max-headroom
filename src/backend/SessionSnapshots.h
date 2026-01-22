#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "backend/EqConfig.h"
#include "backend/PatchbayProfiles.h"

class PipeWireGraph;
class QSettings;

struct SessionSnapshot final {
  QString name;

  QVector<PatchbayLinkSpec> links;

  QString defaultSinkName;
  QString defaultSourceName;

  QHash<QString, EqPreset> eqByNodeName;

  QStringList sinksOrder;
  QHash<QString, QString> patchbayPositionByNodeName; // node.name -> "x,y"
};

struct SessionSnapshotApplyResult final {
  PatchbayProfileApplyResult patchbay;

  bool defaultSinkRequested = false;
  bool defaultSinkSet = false;
  bool defaultSourceRequested = false;
  bool defaultSourceSet = false;

  QStringList missing;
  QStringList errors;
};

class SessionSnapshotStore final
{
public:
  static QStringList listSnapshotNames(QSettings& s);
  static std::optional<SessionSnapshot> load(QSettings& s, const QString& snapshotName);
  static void save(QSettings& s, const SessionSnapshot& snapshot);
  static bool remove(QSettings& s, const QString& snapshotName);
};

SessionSnapshot snapshotSession(const QString& snapshotName, const PipeWireGraph& graph, QSettings& s);
SessionSnapshotApplyResult applySessionSnapshot(PipeWireGraph& graph,
                                               QSettings& s,
                                               const SessionSnapshot& snapshot,
                                               bool strictLinks,
                                               bool strictSettings);

