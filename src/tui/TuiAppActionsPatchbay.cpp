#include "tui/TuiAppInternal.h"

#include "backend/PatchbayPortConfig.h"

#include <QSet>

#include <algorithm>

#include <curses.h>

namespace headroomtui {
namespace tui_actions_internal {

void handlePatchbayKey(int ch, PipeWireGraph& graph, TuiState& state)
{
  QList<PwLinkInfo> links = graph.links();
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

  QSettings portSettings;
  const QList<PwNodeInfo> nodesAll = graph.nodes();
  const QList<PwPortInfo> portsAll = graph.ports();

  QHash<uint32_t, PwNodeInfo> nodesByIdAll;
  nodesByIdAll.reserve(nodesAll.size());
  for (const auto& n : nodesAll) {
    nodesByIdAll.insert(n.id, n);
  }

  QHash<uint32_t, PwPortInfo> portsByIdAll;
  portsByIdAll.reserve(portsAll.size());
  for (const auto& p : portsAll) {
    portsByIdAll.insert(p.id, p);
  }

  auto portLocked = [&](uint32_t portId) -> bool {
    const auto it = portsByIdAll.find(portId);
    if (it == portsByIdAll.end()) {
      return false;
    }
    const PwPortInfo port = it.value();
    const PwNodeInfo node = nodesByIdAll.value(port.nodeId, PwNodeInfo{});
    return PatchbayPortConfigStore::isLocked(portSettings, node.name, port.name);
  };

  state.selectedLink = clampIndex(state.selectedLink, links.size());

  switch (ch) {
  case KEY_UP:
    state.selectedLink = clampIndex(state.selectedLink - 1, links.size());
    break;
  case KEY_DOWN:
    state.selectedLink = clampIndex(state.selectedLink + 1, links.size());
    break;
  case 'd':
  case 'D':
  case KEY_DC:
  case 127: // backspace/delete on some terminals
    if (!links.isEmpty()) {
      const PwLinkInfo link = links[state.selectedLink];
      if (portLocked(link.outputPortId) || portLocked(link.inputPortId)) {
        state.patchbayStatus = QStringLiteral("Cannot delete: port locked.");
        beep();
      } else {
        const bool ok = graph.destroyLink(link.id);
        if (!ok) {
          state.patchbayStatus = QStringLiteral("Delete failed for link %1.").arg(link.id);
          beep();
        } else {
          state.patchbayStatus = QStringLiteral("Deleted link %1.").arg(link.id);
        }
      }
    } else {
      beep();
    }
    break;
  case 'c':
  case 'C':
  case '\n':
  case KEY_ENTER: {
    int height = 0;
    int width = 0;
    getmaxyx(stdscr, height, width);

    const QList<PwNodeInfo> nodes = graph.nodes();
    const QList<PwPortInfo> ports = graph.ports();

    QHash<uint32_t, PwNodeInfo> nodesById;
    nodesById.reserve(nodes.size());
    for (const auto& n : nodes) {
      nodesById.insert(n.id, n);
    }

    const QList<PwNodeInfo> outNodes = nodesWithPortDirection(nodes, ports, QStringLiteral("out"));
    const uint32_t outNodeId = promptSelectNodeId("Select output node", outNodes, state.patchbayOutNodeId, height, width);
    if (outNodeId == 0) {
      break;
    }

    const QList<PwPortInfo> outPorts = portsForNode(ports, outNodeId, QStringLiteral("out"));
    const uint32_t outPortId = promptSelectPortId("Select output port", outPorts, state.patchbayOutPortId, height, width, nodesById);
    if (outPortId == 0) {
      break;
    }
    if (portLocked(outPortId)) {
      state.patchbayStatus = QStringLiteral("Cannot connect: output port locked.");
      beep();
      break;
    }

    PortKind wantKind = PortKind::Other;
    for (const auto& p : outPorts) {
      if (p.id == outPortId) {
        wantKind = portKindFor(p, nodesById);
        break;
      }
    }

    QSet<uint32_t> allowedInNodes;
    allowedInNodes.reserve(ports.size());
    for (const auto& p : ports) {
      if (p.direction == QStringLiteral("in") && p.nodeId != 0 && portKindFor(p, nodesById) == wantKind) {
        allowedInNodes.insert(p.nodeId);
      }
    }

    QList<PwNodeInfo> inNodes;
    inNodes.reserve(nodes.size());
    for (const auto& n : nodes) {
      if (allowedInNodes.contains(n.id)) {
        inNodes.push_back(n);
      }
    }
    std::sort(inNodes.begin(), inNodes.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) {
      if (a.mediaClass != b.mediaClass) {
        return a.mediaClass < b.mediaClass;
      }
      if (a.description != b.description) {
        return a.description < b.description;
      }
      return a.name < b.name;
    });

    if (inNodes.isEmpty()) {
      state.patchbayStatus = QStringLiteral("No compatible input nodes (need %1).").arg(QString::fromUtf8(portKindLabelShort(wantKind)));
      beep();
      break;
    }

    const QString inTitle = QStringLiteral("Select input node (%1)").arg(QString::fromUtf8(portKindLabelShort(wantKind)));
    const QByteArray inTitleUtf8 = inTitle.toUtf8();
    const uint32_t inNodeId = promptSelectNodeId(inTitleUtf8.constData(), inNodes, state.patchbayInNodeId, height, width);
    if (inNodeId == 0) {
      break;
    }

    QList<PwPortInfo> inPortsAll = portsForNode(ports, inNodeId, QStringLiteral("in"));
    QList<PwPortInfo> inPorts;
    inPorts.reserve(inPortsAll.size());
    for (const auto& p : inPortsAll) {
      if (portKindFor(p, nodesById) == wantKind) {
        inPorts.push_back(p);
      }
    }
    if (inPorts.isEmpty()) {
      state.patchbayStatus = QStringLiteral("No compatible input ports.");
      beep();
      break;
    }

    const uint32_t inPortId = promptSelectPortId("Select input port", inPorts, state.patchbayInPortId, height, width, nodesById);
    if (inPortId == 0) {
      break;
    }
    if (portLocked(inPortId)) {
      state.patchbayStatus = QStringLiteral("Cannot connect: input port locked.");
      beep();
      break;
    }

    state.patchbayOutNodeId = outNodeId;
    state.patchbayOutPortId = outPortId;
    state.patchbayInNodeId = inNodeId;
    state.patchbayInPortId = inPortId;

    bool exists = false;
    for (const auto& l : graph.links()) {
      if (l.outputNodeId == outNodeId && l.outputPortId == outPortId && l.inputNodeId == inNodeId && l.inputPortId == inPortId) {
        exists = true;
        break;
      }
    }

    if (exists) {
      state.patchbayStatus = QStringLiteral("Already connected.");
    } else {
      const bool ok = graph.createLink(outNodeId, outPortId, inNodeId, inPortId);
      if (!ok) {
        state.patchbayStatus = QStringLiteral("Connect failed.");
        beep();
      } else {
        state.patchbayStatus = QStringLiteral("Connected.");
      }
    }
    break;
  }
  default:
    break;
  }
}

} // namespace tui_actions_internal
} // namespace headroomtui

