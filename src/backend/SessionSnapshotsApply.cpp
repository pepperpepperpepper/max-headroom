#include "SessionSnapshots.h"

#include "backend/PipeWireGraph.h"
#include "settings/SettingsKeys.h"

#include <QJsonDocument>
#include <QSettings>

namespace {
QString eqPresetKeyForNodeName(const QString& nodeName)
{
  return QStringLiteral("eq/%1/presetJson").arg(nodeName);
}

std::optional<uint32_t> findNodeIdByName(const PipeWireGraph& graph, const QString& nodeName)
{
  if (nodeName.trimmed().isEmpty()) {
    return std::nullopt;
  }
  const auto nodes = graph.nodes();
  for (const auto& n : nodes) {
    if (n.name == nodeName) {
      return n.id;
    }
  }
  return std::nullopt;
}
} // namespace

SessionSnapshotApplyResult applySessionSnapshot(PipeWireGraph& graph,
                                               QSettings& s,
                                               const SessionSnapshot& snapshot,
                                               bool strictLinks,
                                               bool strictSettings)
{
  SessionSnapshotApplyResult res;

  // Apply patchbay links first.
  PatchbayProfile p;
  p.name = snapshot.name;
  p.links = snapshot.links;
  res.patchbay = applyPatchbayProfile(graph, p, strictLinks);
  res.missing.append(res.patchbay.missing);
  res.errors.append(res.patchbay.errors);

  // Restore defaults (best-effort).
  if (!snapshot.defaultSinkName.trimmed().isEmpty()) {
    res.defaultSinkRequested = true;
    if (!graph.hasDefaultDeviceSupport()) {
      res.errors.push_back(QStringLiteral("PipeWire metadata unavailable; cannot set default output"));
    } else if (const auto id = findNodeIdByName(graph, snapshot.defaultSinkName)) {
      const bool ok = graph.setDefaultAudioSink(*id);
      if (!ok) {
        res.errors.push_back(QStringLiteral("Failed to set default output to %1").arg(snapshot.defaultSinkName));
      } else {
        res.defaultSinkSet = true;
      }
    } else {
      res.missing.push_back(QStringLiteral("Default output node missing: %1").arg(snapshot.defaultSinkName));
    }
  }

  if (!snapshot.defaultSourceName.trimmed().isEmpty()) {
    res.defaultSourceRequested = true;
    if (!graph.hasDefaultDeviceSupport()) {
      res.errors.push_back(QStringLiteral("PipeWire metadata unavailable; cannot set default input"));
    } else if (const auto id = findNodeIdByName(graph, snapshot.defaultSourceName)) {
      const bool ok = graph.setDefaultAudioSource(*id);
      if (!ok) {
        res.errors.push_back(QStringLiteral("Failed to set default input to %1").arg(snapshot.defaultSourceName));
      } else {
        res.defaultSourceSet = true;
      }
    } else {
      res.missing.push_back(QStringLiteral("Default input node missing: %1").arg(snapshot.defaultSourceName));
    }
  }

  // Restore settings (EQ + layout).
  if (strictSettings) {
    s.remove(SettingsKeys::sinksOrder());
    s.remove(SettingsKeys::patchbayLayoutPositionsGroup());
    s.remove(QStringLiteral("eq"));
  }

  if (!snapshot.sinksOrder.isEmpty()) {
    s.setValue(SettingsKeys::sinksOrder(), snapshot.sinksOrder);
  } else if (strictSettings) {
    s.remove(SettingsKeys::sinksOrder());
  }

  for (auto it = snapshot.patchbayPositionByNodeName.begin(); it != snapshot.patchbayPositionByNodeName.end(); ++it) {
    const QString nodeName = it.key();
    const QString pos = it.value();
    if (nodeName.trimmed().isEmpty() || pos.trimmed().isEmpty()) {
      continue;
    }
    s.setValue(SettingsKeys::patchbayLayoutPositionKeyForNodeName(nodeName), pos);
  }

  for (auto it = snapshot.eqByNodeName.begin(); it != snapshot.eqByNodeName.end(); ++it) {
    const QString nodeName = it.key();
    const EqPreset preset = it.value();
    if (nodeName.trimmed().isEmpty()) {
      continue;
    }
    const QString json = QString::fromUtf8(QJsonDocument(eqPresetToJson(preset)).toJson(QJsonDocument::Compact));
    s.setValue(eqPresetKeyForNodeName(nodeName), json);
  }

  return res;
}

