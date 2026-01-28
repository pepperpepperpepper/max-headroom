#include "SessionSnapshots.h"

#include "settings/SettingsKeys.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

namespace {
constexpr const char* kSnapshotDisplayNameKey = "displayName";
constexpr const char* kSnapshotJsonKey = "snapshotJson";

QString snapshotIdForName(const QString& snapshotName)
{
  const QByteArray enc = snapshotName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
  return QString::fromUtf8(enc);
}

std::optional<QString> findSnapshotIdByDisplayName(QSettings& s, const QString& snapshotName)
{
  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
    s.endGroup();
    if (name == snapshotName) {
      s.endGroup();
      return g;
    }
  }
  s.endGroup();
  return std::nullopt;
}

QJsonArray linksToJson(const QVector<PatchbayLinkSpec>& links)
{
  QJsonArray arr;
  for (const auto& l : links) {
    QJsonObject o;
    o.insert(QStringLiteral("outputNode"), l.outputNodeName);
    o.insert(QStringLiteral("outputPort"), l.outputPortName);
    o.insert(QStringLiteral("inputNode"), l.inputNodeName);
    o.insert(QStringLiteral("inputPort"), l.inputPortName);
    arr.append(o);
  }
  return arr;
}

QVector<PatchbayLinkSpec> linksFromJson(const QJsonArray& arr)
{
  QVector<PatchbayLinkSpec> links;
  links.reserve(arr.size());
  for (const auto& v : arr) {
    if (!v.isObject()) {
      continue;
    }
    const QJsonObject o = v.toObject();
    PatchbayLinkSpec l;
    l.outputNodeName = o.value(QStringLiteral("outputNode")).toString();
    l.outputPortName = o.value(QStringLiteral("outputPort")).toString();
    l.inputNodeName = o.value(QStringLiteral("inputNode")).toString();
    l.inputPortName = o.value(QStringLiteral("inputPort")).toString();
    if (l.outputNodeName.isEmpty() || l.outputPortName.isEmpty() || l.inputNodeName.isEmpty() || l.inputPortName.isEmpty()) {
      continue;
    }
    links.push_back(l);
  }
  return links;
}

QJsonObject eqMapToJson(const QHash<QString, EqPreset>& eqByNodeName)
{
  QJsonObject o;
  for (auto it = eqByNodeName.begin(); it != eqByNodeName.end(); ++it) {
    o.insert(it.key(), eqPresetToJson(it.value()));
  }
  return o;
}

QHash<QString, EqPreset> eqMapFromJson(const QJsonObject& o)
{
  QHash<QString, EqPreset> out;
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (!it.value().isObject()) {
      continue;
    }
    out.insert(it.key(), eqPresetFromJson(it.value().toObject()));
  }
  return out;
}

QJsonObject positionsToJson(const QHash<QString, QString>& posByNodeName)
{
  QJsonObject o;
  for (auto it = posByNodeName.begin(); it != posByNodeName.end(); ++it) {
    o.insert(it.key(), it.value());
  }
  return o;
}

QHash<QString, QString> positionsFromJson(const QJsonObject& o)
{
  QHash<QString, QString> out;
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (!it.value().isString()) {
      continue;
    }
    const QString v = it.value().toString();
    if (!v.trimmed().isEmpty()) {
      out.insert(it.key(), v);
    }
  }
  return out;
}

QJsonObject snapshotToJson(const SessionSnapshot& snapshot)
{
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);

  QJsonObject patchbay;
  patchbay.insert(QStringLiteral("links"), linksToJson(snapshot.links));
  root.insert(QStringLiteral("patchbay"), patchbay);

  QJsonObject defaults;
  defaults.insert(QStringLiteral("sink"), snapshot.defaultSinkName);
  defaults.insert(QStringLiteral("source"), snapshot.defaultSourceName);
  root.insert(QStringLiteral("defaults"), defaults);

  root.insert(QStringLiteral("eq"), eqMapToJson(snapshot.eqByNodeName));

  QJsonObject layout;
  QJsonArray order;
  for (const auto& n : snapshot.sinksOrder) {
    order.append(n);
  }
  layout.insert(QStringLiteral("sinksOrder"), order);
  layout.insert(QStringLiteral("patchbayPositions"), positionsToJson(snapshot.patchbayPositionByNodeName));
  root.insert(QStringLiteral("layout"), layout);

  return root;
}

SessionSnapshot snapshotFromJson(const QString& snapshotName, const QJsonObject& root)
{
  SessionSnapshot snapshot;
  snapshot.name = snapshotName.trimmed();

  const QJsonObject patchbay = root.value(QStringLiteral("patchbay")).toObject();
  const QJsonArray linksArr = patchbay.value(QStringLiteral("links")).toArray();
  snapshot.links = linksFromJson(linksArr);

  const QJsonObject defaults = root.value(QStringLiteral("defaults")).toObject();
  snapshot.defaultSinkName = defaults.value(QStringLiteral("sink")).toString();
  snapshot.defaultSourceName = defaults.value(QStringLiteral("source")).toString();

  snapshot.eqByNodeName = eqMapFromJson(root.value(QStringLiteral("eq")).toObject());

  const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
  const QJsonArray sinksOrder = layout.value(QStringLiteral("sinksOrder")).toArray();
  for (const auto& v : sinksOrder) {
    if (v.isString()) {
      snapshot.sinksOrder.push_back(v.toString());
    }
  }
  snapshot.patchbayPositionByNodeName = positionsFromJson(layout.value(QStringLiteral("patchbayPositions")).toObject());
  return snapshot;
}
} // namespace

QStringList SessionSnapshotStore::listSnapshotNames(QSettings& s)
{
  QStringList names;
  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  const QStringList groups = s.childGroups();
  for (const auto& g : groups) {
    s.beginGroup(g);
    const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
    s.endGroup();
    if (!name.trimmed().isEmpty()) {
      names.push_back(name);
    }
  }
  s.endGroup();

  std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
  return names;
}

std::optional<SessionSnapshot> SessionSnapshotStore::load(QSettings& s, const QString& snapshotName)
{
  const auto idOpt = findSnapshotIdByDisplayName(s, snapshotName);
  if (!idOpt) {
    return std::nullopt;
  }

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.beginGroup(*idOpt);
  const QString name = s.value(QString::fromUtf8(kSnapshotDisplayNameKey)).toString();
  const QString json = s.value(QString::fromUtf8(kSnapshotJsonKey)).toString();
  s.endGroup();
  s.endGroup();

  if (name.trimmed().isEmpty() || json.trimmed().isEmpty()) {
    return std::nullopt;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return std::nullopt;
  }

  return snapshotFromJson(name, doc.object());
}

void SessionSnapshotStore::save(QSettings& s, const SessionSnapshot& snapshot)
{
  const QString name = snapshot.name.trimmed();
  if (name.isEmpty()) {
    return;
  }

  const QString id = snapshotIdForName(name);

  const QString json = QString::fromUtf8(QJsonDocument(snapshotToJson(snapshot)).toJson(QJsonDocument::Compact));

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.beginGroup(id);
  s.setValue(QString::fromUtf8(kSnapshotDisplayNameKey), name);
  s.setValue(QString::fromUtf8(kSnapshotJsonKey), json);
  s.endGroup();
  s.endGroup();
}

bool SessionSnapshotStore::remove(QSettings& s, const QString& snapshotName)
{
  const auto idOpt = findSnapshotIdByDisplayName(s, snapshotName);
  if (!idOpt) {
    return false;
  }

  s.beginGroup(SettingsKeys::sessionsSnapshotsGroup());
  s.remove(*idOpt);
  s.endGroup();
  return true;
}

