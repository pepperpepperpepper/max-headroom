#pragma once

#include "backend/PipeWireGraph.h"

#include <optional>

#include <QColor>
#include <QPen>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace patchbayui {
inline constexpr int kDataPortId = 0;
inline constexpr int kDataNodeId = 1;
inline constexpr int kDataPortDir = 2;  // 0=out, 1=in
inline constexpr int kDataNodeName = 3;
inline constexpr int kDataPortKind = 4; // 0=audio, 1=midi, 2=other
inline constexpr int kDataPortName = 5;
inline constexpr int kDataPortLocked = 6;
inline constexpr int kDataLinkId = 10;

enum class PortKind {
  Audio = 0,
  Midi = 1,
  Other = 2,
};

inline bool isInternalNode(const PwNodeInfo& node)
{
  return node.name.startsWith(QStringLiteral("headroom.meter.")) || node.name == QStringLiteral("headroom.visualizer") ||
      node.name == QStringLiteral("headroom.recorder");
}

inline QColor outColor() { return QColor(99, 102, 241); }
inline QColor outSelectedColor() { return QColor(225, 231, 255); }
inline QColor outHoverColor() { return QColor(165, 180, 252); }
inline QColor inColor() { return QColor(34, 211, 238); }
inline QColor inHoverColor() { return QColor(103, 232, 249); }
inline QColor midiOutColor() { return QColor(249, 115, 22); }
inline QColor midiOutHoverColor() { return QColor(251, 146, 60); }
inline QColor midiInColor() { return QColor(34, 197, 94); }
inline QColor midiInHoverColor() { return QColor(74, 222, 128); }
inline QColor otherOutColor() { return QColor(148, 163, 184); }
inline QColor otherOutHoverColor() { return QColor(203, 213, 225); }
inline QColor otherInColor() { return QColor(148, 163, 184); }
inline QColor otherInHoverColor() { return QColor(203, 213, 225); }
inline QPen linkPen() { return QPen(QColor(148, 163, 184, 150), 2); }
inline QPen linkHoverPen() { return QPen(QColor(226, 232, 240, 200), 3); }
inline QPen linkSelectedPen() { return QPen(QColor(56, 189, 248, 230), 3); }
inline QPen dragWirePen()
{
  QPen p(QColor(99, 102, 241, 210), 2);
  p.setStyle(Qt::DashLine);
  return p;
}

inline std::optional<QPointF> parsePoint(const QString& s)
{
  const QStringList parts = s.split(',', Qt::SkipEmptyParts);
  if (parts.size() != 2) {
    return std::nullopt;
  }
  bool okX = false;
  bool okY = false;
  const double x = parts[0].toDouble(&okX);
  const double y = parts[1].toDouble(&okY);
  if (!okX || !okY) {
    return std::nullopt;
  }
  return QPointF(x, y);
}

inline QString formatPoint(const QPointF& p)
{
  return QStringLiteral("%1,%2").arg(qRound(p.x())).arg(qRound(p.y()));
}

inline bool isRedundantChannelLabel(const QString& label, const QString& ch)
{
  if (label.trimmed().isEmpty() || ch.trimmed().isEmpty()) {
    return true;
  }

  const QString l = label.trimmed().toLower();
  const QString c = ch.trimmed().toLower();
  if (l == c) {
    return true;
  }

  return l.endsWith(QStringLiteral("_%1").arg(c)) || l.endsWith(QStringLiteral("-%1").arg(c)) || l.endsWith(QStringLiteral(" %1").arg(c));
}

inline PortKind portKindFor(const PwPortInfo& p, const PwNodeInfo& node)
{
  const QString mt = p.mediaType.trimmed().toLower();
  if (mt == QStringLiteral("midi")) {
    return PortKind::Midi;
  }
  if (mt == QStringLiteral("audio")) {
    return PortKind::Audio;
  }
  if (!p.audioChannel.isEmpty()) {
    return PortKind::Audio;
  }
  if (p.formatDsp.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (p.formatDsp.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  if (node.mediaClass.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  if (node.mediaClass.contains(QStringLiteral("audio"), Qt::CaseInsensitive)) {
    return PortKind::Audio;
  }
  if (p.name.contains(QStringLiteral("midi"), Qt::CaseInsensitive) || p.alias.contains(QStringLiteral("midi"), Qt::CaseInsensitive)) {
    return PortKind::Midi;
  }
  return PortKind::Other;
}

inline QColor outColorFor(PortKind kind, bool hover)
{
  switch (kind) {
    case PortKind::Audio:
      return hover ? outHoverColor() : outColor();
    case PortKind::Midi:
      return hover ? midiOutHoverColor() : midiOutColor();
    case PortKind::Other:
      return hover ? otherOutHoverColor() : otherOutColor();
  }
  return hover ? outHoverColor() : outColor();
}

inline QColor inColorFor(PortKind kind, bool hover)
{
  switch (kind) {
    case PortKind::Audio:
      return hover ? inHoverColor() : inColor();
    case PortKind::Midi:
      return hover ? midiInHoverColor() : midiInColor();
    case PortKind::Other:
      return hover ? otherInHoverColor() : otherInColor();
  }
  return hover ? inHoverColor() : inColor();
}

inline std::optional<PwPortInfo> portById(const QList<PwPortInfo>& ports, uint32_t portId)
{
  for (const auto& p : ports) {
    if (p.id == portId) {
      return p;
    }
  }
  return std::nullopt;
}

inline QString nodeLabelFor(const PwNodeInfo& n)
{
  if (!n.description.isEmpty()) {
    return n.description;
  }
  if (!n.name.isEmpty()) {
    return n.name;
  }
  return QStringLiteral("(unnamed)");
}

inline int audioChannelRank(const QString& ch)
{
  const QString c = ch.trimmed().toUpper();
  if (c == QStringLiteral("FL")) {
    return 0;
  }
  if (c == QStringLiteral("FR")) {
    return 1;
  }
  if (c == QStringLiteral("FC")) {
    return 2;
  }
  if (c == QStringLiteral("LFE")) {
    return 3;
  }
  if (c == QStringLiteral("RL")) {
    return 4;
  }
  if (c == QStringLiteral("RR")) {
    return 5;
  }
  if (c == QStringLiteral("SL")) {
    return 6;
  }
  if (c == QStringLiteral("SR")) {
    return 7;
  }
  return 1'000'000;
}

inline QString portSortKey(const PwPortInfo& p)
{
  if (!p.audioChannel.isEmpty()) {
    return p.audioChannel.trimmed().toLower();
  }
  if (!p.name.isEmpty()) {
    return p.name.trimmed().toLower();
  }
  return p.alias.trimmed().toLower();
}

inline std::optional<uint32_t> linkIdByPorts(const QList<PwLinkInfo>& links, uint32_t outPortId, uint32_t inPortId)
{
  for (const auto& l : links) {
    if (l.outputPortId == outPortId && l.inputPortId == inPortId) {
      return l.id;
    }
  }
  return std::nullopt;
}
} // namespace patchbayui
