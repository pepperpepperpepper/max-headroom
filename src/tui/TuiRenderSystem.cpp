#include "tui/TuiInternal.h"

#include "backend/PatchbayPortConfig.h"

#include <algorithm>
#include <cmath>

#include <curses.h>

namespace headroomtui {

static QString displayNameForPort(const PwPortInfo& p)
{
  if (!p.name.isEmpty()) {
    return p.name;
  }
  if (!p.alias.isEmpty()) {
    return p.alias;
  }
  return QStringLiteral("(unnamed)");
}

static QString displayNameForPort(const PwPortInfo& p, const QString& nodeName, QSettings& settings)
{
  if (const auto custom = PatchbayPortConfigStore::customAlias(settings, nodeName, p.name)) {
    return *custom;
  }
  return displayNameForPort(p);
}

uint32_t promptSelectPortId(const char* title,
                            const QList<PwPortInfo>& ports,
                            uint32_t currentId,
                            int height,
                            int width,
                            const QHash<uint32_t, PwNodeInfo>& nodesById)
{
  const int portCount = static_cast<int>(ports.size());
  if (!title || portCount <= 0 || height <= 0 || width <= 0) {
    return 0;
  }

  QSettings portSettings;

  int selected = 0;
  if (currentId != 0) {
    for (int i = 0; i < portCount; ++i) {
      if (ports[i].id == currentId) {
        selected = i;
        break;
      }
    }
  }

  timeout(-1);

  while (true) {
    erase();
    mvprintw(0, 0, "%s", title);
    mvprintw(1, 0, "Up/Down select  Enter confirm  Esc cancel");

    const int listTop = 3;
    const int listHeight = std::max(0, height - listTop - 2);

    selected = clampIndex(selected, portCount);

    int start = 0;
    if (selected >= listHeight) {
      start = selected - listHeight + 1;
    }
    start = std::clamp(start, 0, std::max(0, portCount - listHeight));

    for (int row = 0; row < listHeight; ++row) {
      const int idx = start + row;
      if (idx >= portCount) {
        break;
      }
      const bool isSel = (idx == selected);
      if (isSel) {
        attron(A_REVERSE);
      }

      const auto& p = ports[idx];
      const QString nodeName = nodesById.value(p.nodeId, PwNodeInfo{}).name;
      const QString name = displayNameForPort(p, nodeName, portSettings);

      const QByteArray chanUtf8 = p.audioChannel.toUtf8();
      const QByteArray nameUtf8 = name.toUtf8();

      mvprintw(listTop + row, 0, "%c %4u  ", isSel ? '>' : ' ', p.id);
      if (!p.audioChannel.isEmpty()) {
        mvprintw(listTop + row, 8, "%-6s  ", chanUtf8.constData());
      } else {
        const PortKind kind = portKindFor(p, nodesById);
        mvprintw(listTop + row, 8, "%-6s  ", portKindLabelShort(kind));
      }
      mvaddnstr(listTop + row, 17, nameUtf8.constData(), std::max(0, width - 19));

      if (isSel) {
        attroff(A_REVERSE);
      }
    }

    refresh();

    const int ch = getch();
    switch (ch) {
    case 27: // Esc
    case 'q':
    case 'Q':
      timeout(kMainLoopTimeoutMs);
      return 0;
    case KEY_UP:
    case 'k':
    case 'K':
      selected = clampIndex(selected - 1, portCount);
      break;
    case KEY_DOWN:
    case 'j':
    case 'J':
      selected = clampIndex(selected + 1, portCount);
      break;
    case '\n':
    case KEY_ENTER:
      timeout(kMainLoopTimeoutMs);
      return ports[selected].id;
    default:
      break;
    }
  }
}

void drawPatchbayPage(PipeWireGraph* graph, int& selectedLinkIdx, const QString& statusLine, int height, int width)
{
  mvprintw(3, 0, "Patchbay");

  if (!graph) {
    mvprintw(6, 0, "(no graph)");
    return;
  }

  const QList<PwNodeInfo> nodes = graph->nodes();
  const QList<PwPortInfo> ports = graph->ports();
  QList<PwLinkInfo> links = graph->links();

  QSettings portSettings;

  std::sort(links.begin(), links.end(), [](const PwLinkInfo& a, const PwLinkInfo& b) {
    if (a.outputNodeId != b.outputNodeId) {
      return a.outputNodeId < b.outputNodeId;
    }
    if (a.inputNodeId != b.inputNodeId) {
      return a.inputNodeId < b.inputNodeId;
    }
    if (a.outputPortId != b.outputPortId) {
      return a.outputPortId < b.outputPortId;
    }
    return a.inputPortId < b.inputPortId;
  });

  mvprintw(4,
           0,
           "Nodes: %d  Ports: %d  Links: %d",
           static_cast<int>(nodes.size()),
           static_cast<int>(ports.size()),
           static_cast<int>(links.size()));
  mvprintw(5, 0, "c/Enter connect  d delete link  Up/Down select");

  if (!statusLine.isEmpty()) {
    const QByteArray statusUtf8 = statusLine.toUtf8();
    mvaddnstr(6, 0, statusUtf8.constData(), std::max(0, width - 1));
  }

  const int linkCount = static_cast<int>(links.size());
  const int listTop = 8;
  const int listHeight = std::max(0, height - listTop - 2);

  if (linkCount <= 0) {
    mvprintw(listTop, 0, "(no links)");
    return;
  }

  QHash<uint32_t, QString> nodeNames;
  nodeNames.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodeNames.insert(n.id, displayNameForNode(n));
  }

  QHash<uint32_t, PwNodeInfo> nodesById;
  nodesById.reserve(nodes.size());
  for (const auto& n : nodes) {
    nodesById.insert(n.id, n);
  }

  QHash<uint32_t, QString> portNames;
  portNames.reserve(ports.size());
  for (const auto& p : ports) {
    const QString nodeName = nodesById.value(p.nodeId, PwNodeInfo{}).name;
    portNames.insert(p.id, displayNameForPort(p, nodeName, portSettings));
  }

  selectedLinkIdx = clampIndex(selectedLinkIdx, linkCount);

  int start = 0;
  if (selectedLinkIdx >= listHeight) {
    start = selectedLinkIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, linkCount - listHeight));

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= linkCount) {
      break;
    }

    const auto& l = links[idx];
    const bool selected = (idx == selectedLinkIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString outNode = nodeNames.value(l.outputNodeId, QStringLiteral("node %1").arg(l.outputNodeId));
    const QString outPort = portNames.value(l.outputPortId, QStringLiteral("port %1").arg(l.outputPortId));
    const QString inNode = nodeNames.value(l.inputNodeId, QStringLiteral("node %1").arg(l.inputNodeId));
    const QString inPort = portNames.value(l.inputPortId, QStringLiteral("port %1").arg(l.inputPortId));
    const QString text = QStringLiteral("%1:%2 -> %3:%4").arg(outNode, outPort, inNode, inPort);
    const QByteArray textUtf8 = text.toUtf8();

    const int y = listTop + row;
    mvprintw(y, 0, "%c %4u  ", selected ? '>' : ' ', l.id);
    mvaddnstr(y, 8, textUtf8.constData(), std::max(0, width - 10));

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

void drawStatusPage(PipeWireGraph* graph, int& selectedIdx, int height, int width)
{
  mvprintw(3, 0, "Status / diagnostics");

  if (!graph || !graph->hasProfilerSupport()) {
    mvprintw(5, 0, "Profiler unavailable (PipeWire module-profiler not loaded).");
    return;
  }

  const auto snapOpt = graph->profilerSnapshot();
  if (!snapOpt.has_value() || snapOpt->seq == 0) {
    mvprintw(5, 0, "Waiting for profiler data...");
    return;
  }

  const PwProfilerSnapshot s = *snapOpt;

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

  if (s.hasInfo) {
    const QString cpu = QStringLiteral("CPU load: %1 / %2 / %3   XRuns: %4")
                            .arg(fmtPct(s.cpuLoadFast), fmtPct(s.cpuLoadMedium), fmtPct(s.cpuLoadSlow))
                            .arg(s.xrunCount);
    const QByteArray cpuUtf8 = cpu.toUtf8();
    mvaddnstr(5, 0, cpuUtf8.constData(), width);
  } else {
    mvprintw(5, 0, "Profiler info unavailable.");
  }

  if (s.hasClock) {
    QStringList bits;
    if (s.clockDurationMs.has_value()) {
      bits << QStringLiteral("duration %1 ms").arg(fmtMs(s.clockDurationMs));
    }
    if (s.clockDelayMs.has_value()) {
      bits << QStringLiteral("delay %1 ms").arg(fmtMs(s.clockDelayMs));
    }
    if (s.clockXrunDurationMs.has_value() && *s.clockXrunDurationMs > 0.0) {
      bits << QStringLiteral("xrun %1 ms").arg(fmtMs(s.clockXrunDurationMs));
    }
    if (s.clockCycle > 0) {
      bits << QStringLiteral("cycle %1").arg(s.clockCycle);
    }
    const QString clock = QStringLiteral("Clock: %1").arg(bits.join(QStringLiteral(", ")));
    const QByteArray clockUtf8 = clock.toUtf8();
    mvaddnstr(6, 0, clockUtf8.constData(), width);
  }

  mvprintw(8, 0, "Drivers:");

  const int listTop = 10;
  const int listHeight = std::max(0, height - listTop - 2);

  if (s.drivers.isEmpty()) {
    mvprintw(listTop, 0, "(no drivers reported)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, s.drivers.size());

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, static_cast<int>(s.drivers.size()) - listHeight));

  mvprintw(listTop - 1, 0, "  ID   LAT(ms)  BUSY    WAIT   XRUN  NAME");

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= s.drivers.size()) {
      break;
    }
    const auto& d = s.drivers[idx];

    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString lat = fmtMs(d.latencyMs);
    const QString busy = d.busyRatio.has_value() ? fmtPct(*d.busyRatio) : QStringLiteral("-");
    const QString wait = d.waitRatio.has_value() ? fmtPct(*d.waitRatio) : QStringLiteral("-");
    const QString name = d.name.isEmpty() ? QStringLiteral("(unnamed)") : d.name;

    const int y = listTop + row;
    mvprintw(y,
             0,
             "%c %4u  %7s  %6s  %6s  %4d  ",
             selected ? '>' : ' ',
             d.id,
             lat.toUtf8().constData(),
             busy.toUtf8().constData(),
             wait.toUtf8().constData(),
             d.xrunCount);

    const QByteArray nameUtf8 = name.toUtf8();
    const int nameX = 0 + 1 + 5 + 2 + 8 + 2 + 7 + 2 + 7 + 2 + 5 + 2;
    mvaddnstr(y, nameX, nameUtf8.constData(), std::max(0, width - nameX - 1));

    if (selected) {
      attroff(A_REVERSE);
    }
  }
}

void drawEnginePage(const QList<SystemdUnitStatus>& units, int& selectedIdx, const QString& engineStatus, int height, int width)
{
  mvprintw(3, 0, "Engine control (systemd --user)");
  mvprintw(4, 0, "Up/Down select  S start  T stop  R restart  g refresh");

  if (!engineStatus.trimmed().isEmpty()) {
    const QByteArray utf8 = engineStatus.toUtf8();
    mvaddnstr(5, 0, utf8.constData(), std::max(0, width - 1));
  }

  const int listTop = 7;
  const int listHeight = std::max(0, height - listTop - 4);
  const int count = units.size();
  if (count <= 0) {
    mvprintw(listTop, 0, "(no units)");
    return;
  }

  selectedIdx = clampIndex(selectedIdx, count);

  int start = 0;
  if (selectedIdx >= listHeight) {
    start = selectedIdx - listHeight + 1;
  }
  start = std::clamp(start, 0, std::max(0, count - listHeight));

  mvprintw(listTop - 1, 0, "  STATE          UNIT  DETAILS");

  for (int row = 0; row < listHeight; ++row) {
    const int idx = start + row;
    if (idx >= count) {
      break;
    }

    const auto& st = units[idx];
    const bool selected = (idx == selectedIdx);
    if (selected) {
      attron(A_REVERSE);
    }

    const QString active = st.activeState.isEmpty() ? QStringLiteral("-") : st.activeState;
    const QString sub = st.subState.isEmpty() ? QStringLiteral("-") : st.subState;
    const QString details = st.error.isEmpty() ? st.description : QStringLiteral("ERR: %1").arg(st.error);
    const QString line = QStringLiteral("%1/%2  %3  %4").arg(active, sub, st.unit, details);
    const QByteArray utf8 = line.toUtf8();

    const int y = listTop + row;
    mvprintw(y, 0, "%c ", selected ? '>' : ' ');
    mvaddnstr(y, 2, utf8.constData(), std::max(0, width - 3));

    if (selected) {
      attroff(A_REVERSE);
    }
  }

  if (height >= 3) {
    const SystemdUnitStatus st = units.value(selectedIdx);
    const QString active = st.activeState.isEmpty() ? QStringLiteral("-") : st.activeState;
    const QString sub = st.subState.isEmpty() ? QStringLiteral("-") : st.subState;
    const QString load = st.loadState.isEmpty() ? QStringLiteral("-") : st.loadState;
    const QString msg = QStringLiteral("Selected: %1  load:%2  active:%3/%4").arg(st.unit, load, active, sub);
    const QByteArray utf8 = msg.toUtf8();
    mvaddnstr(height - 2, 0, utf8.constData(), std::max(0, width - 1));
  }
}

} // namespace headroomtui

