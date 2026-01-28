#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <optional>

#include "settings/SettingsKeys.h"

#include "cli/CliInternal.h"

namespace headroomctl {
int handleGraphListingCommand(const QString& cmd, QStringList args, PipeWireGraph& graph, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
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
      const QString sub = (args.size() >= 3) ? args.at(2).trimmed().toLower() : QString{};
      if (sub == QStringLiteral("order")) {
        const QString action = (args.size() >= 4) ? args.at(3).trimmed().toLower() : QStringLiteral("get");
        const QList<PwNodeInfo> sinks = graph.audioSinks();

        auto defaultOrderNames = [&]() -> QStringList {
          QList<PwNodeInfo> sorted = sinks;
          std::sort(sorted.begin(), sorted.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return nodeLabel(a).toLower() < nodeLabel(b).toLower(); });

          QStringList order;
          order.reserve(sorted.size());
          for (const auto& n : sorted) {
            if (!n.name.isEmpty()) {
              order.push_back(n.name);
            }
          }
          return order;
        };

        auto currentOrderNames = [&](QSettings& s) -> QStringList {
          const QStringList saved = s.value(SettingsKeys::sinksOrder()).toStringList();
          if (saved.isEmpty()) {
            return defaultOrderNames();
          }

          QStringList used;
          used.reserve(saved.size());
          for (const auto& name : saved) {
            for (const auto& n : sinks) {
              if (n.name == name) {
                used.push_back(name);
                break;
              }
            }
          }

          QList<PwNodeInfo> remaining;
          remaining.reserve(sinks.size());
          for (const auto& n : sinks) {
            if (!used.contains(n.name)) {
              remaining.push_back(n);
            }
          }
          std::sort(remaining.begin(), remaining.end(), [](const PwNodeInfo& a, const PwNodeInfo& b) { return nodeLabel(a).toLower() < nodeLabel(b).toLower(); });
          for (const auto& n : remaining) {
            used.push_back(n.name);
          }
          return used;
        };

        auto orderToJson = [&](const QStringList& order) -> QJsonArray {
          QJsonArray arr;
          for (const auto& name : order) {
            for (const auto& n : sinks) {
              if (n.name == name) {
                arr.append(nodeToJson(n));
                break;
              }
            }
          }
          return arr;
        };

        if (action == QStringLiteral("get") || action.isEmpty()) {
          QSettings s;
          const QStringList order = currentOrderNames(s);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("order"), orderToJson(order));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            int i = 1;
            for (const auto& name : order) {
              const auto it = std::find_if(sinks.begin(), sinks.end(), [&](const PwNodeInfo& n) { return n.name == name; });
              if (it == sinks.end()) {
                continue;
              }
              out << i++ << "\t" << it->id << "\t" << nodeLabel(*it) << "\n";
            }
          }
          exitCode = 0;
          break;
        }

        if (action == QStringLiteral("reset")) {
          QSettings s;
          s.remove(SettingsKeys::sinksOrder());
          const QStringList order = currentOrderNames(s);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("order"), orderToJson(order));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "reset\n";
          }
          exitCode = 0;
          break;
        }

        if (action == QStringLiteral("move")) {
          if (args.size() < 6) {
            err << "headroomctl: sinks order move expects <node-id|node-name> up|down|top|bottom\n";
            exitCode = 2;
            break;
          }

          auto resolveSinkName = [&](const QString& idOrName) -> QString {
            const QString key = idOrName.trimmed();
            bool ok = false;
            const uint32_t id = key.toUInt(&ok);
            if (ok) {
              for (const auto& n : sinks) {
                if (n.id == id) {
                  return n.name;
                }
              }
            }

            for (const auto& n : sinks) {
              if (n.name == key || nodeLabel(n) == key || n.description == key) {
                return n.name;
              }
            }
            return {};
          };

          const QString sinkName = resolveSinkName(args.at(4));
          const QString dir = args.at(5).trimmed().toLower();
          if (sinkName.isEmpty()) {
            err << "headroomctl: unknown sink\n";
            exitCode = 2;
            break;
          }

          QSettings s;
          QStringList order = currentOrderNames(s);
          if (!order.contains(sinkName)) {
            order.push_back(sinkName);
          }

          const QStringList def = defaultOrderNames();
          int idx = order.indexOf(sinkName);
          bool moved = false;

          if (dir == QStringLiteral("up")) {
            if (idx > 0) {
              order.swapItemsAt(idx, idx - 1);
              moved = true;
            }
          } else if (dir == QStringLiteral("down")) {
            if (idx >= 0 && idx + 1 < order.size()) {
              order.swapItemsAt(idx, idx + 1);
              moved = true;
            }
          } else if (dir == QStringLiteral("top")) {
            if (idx > 0) {
              order.removeAt(idx);
              order.prepend(sinkName);
              moved = true;
            }
          } else if (dir == QStringLiteral("bottom")) {
            if (idx >= 0 && idx + 1 < order.size()) {
              order.removeAt(idx);
              order.push_back(sinkName);
              moved = true;
            }
          } else {
            err << "headroomctl: sinks order move expects up|down|top|bottom\n";
            exitCode = 2;
            break;
          }

          if (!moved) {
            if (jsonOutput) {
              QJsonObject o;
              o.insert(QStringLiteral("ok"), false);
              o.insert(QStringLiteral("moved"), false);
              o.insert(QStringLiteral("order"), orderToJson(order));
              out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
            } else {
              out << "no-op\n";
            }
            exitCode = 1;
            break;
          }

          const bool storeCustom = !def.isEmpty() && order != def;
          if (storeCustom) {
            s.setValue(SettingsKeys::sinksOrder(), order);
          } else {
            s.remove(SettingsKeys::sinksOrder());
          }

          const QStringList next = currentOrderNames(s);
          if (jsonOutput) {
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("moved"), true);
            o.insert(QStringLiteral("order"), orderToJson(next));
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
          } else {
            out << "moved\n";
          }
          exitCode = 0;
          break;
        }

        err << "headroomctl: sinks order expects get|move|reset\n";
        exitCode = 2;
        break;
      }

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
        std::sort(sorted.begin(),
                  sorted.end(),
                  [](const PwPortInfo& a, const PwPortInfo& b) {
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

          out << l.id << "\t" << l.outputPortId << "\t" << l.inputPortId << "\t" << nodeLabel(outNode) << ":" << outPort.name << " -> " << nodeLabel(inNode)
              << ":" << inPort.name << "\n";
        }
      }
      exitCode = 0;
      break;
    }
  } while (false);

  return exitCode;
}
} // namespace headroomctl

