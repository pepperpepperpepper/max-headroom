#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class PipeWireGraph;
class QSettings;

struct PatchbayLinkSpec final {
  QString outputNodeName;
  QString outputPortName;
  QString inputNodeName;
  QString inputPortName;
};

struct PatchbayProfile final {
  QString name;
  QVector<PatchbayLinkSpec> links;
};

struct PatchbayProfileApplyResult final {
  int desiredLinks = 0;
  int createdLinks = 0;
  int alreadyPresentLinks = 0;
  int missingEndpoints = 0;
  int disconnectedLinks = 0;
  QStringList missing;
  QStringList errors;
};

class PatchbayProfileStore final
{
public:
  static QStringList listProfileNames(QSettings& s);
  static std::optional<PatchbayProfile> load(QSettings& s, const QString& profileName);
  static void save(QSettings& s, const PatchbayProfile& profile);
  static bool remove(QSettings& s, const QString& profileName);
};

PatchbayProfile snapshotPatchbayProfile(const QString& profileName, const PipeWireGraph& graph);
PatchbayProfileApplyResult applyPatchbayProfile(PipeWireGraph& graph, const PatchbayProfile& profile, bool strict);

