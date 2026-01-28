#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QTextStream>

#include <cstdint>
#include <optional>

#include "backend/PipeWireThread.h"

#include "cli/CliInternal.h"

namespace headroomctl {
int handleEngineClockCommand(QStringList args, bool jsonOutput, QTextStream& out, QTextStream& err)
{
  int exitCode = 0;
  do {
    const QString clockSub = (args.size() >= 4) ? args.at(3).trimmed().toLower() : QStringLiteral("status");

    PipeWireThread pw;
    PipeWireGraph graph(&pw);
    if (!pw.isConnected()) {
      err << "headroomctl: failed to connect to PipeWire\n";
      exitCode = 1;
      break;
    }

    waitForGraph(250);

    auto settingsToJson = [](const PwClockSettings& s) {
      QJsonObject o;
      if (s.rate.has_value()) {
        o.insert(QStringLiteral("rate"), static_cast<qint64>(*s.rate));
      } else {
        o.insert(QStringLiteral("rate"), QJsonValue());
      }
      if (s.quantum.has_value()) {
        o.insert(QStringLiteral("quantum"), static_cast<qint64>(*s.quantum));
      } else {
        o.insert(QStringLiteral("quantum"), QJsonValue());
      }
      if (s.minQuantum.has_value()) {
        o.insert(QStringLiteral("minQuantum"), static_cast<qint64>(*s.minQuantum));
      } else {
        o.insert(QStringLiteral("minQuantum"), QJsonValue());
      }
      if (s.maxQuantum.has_value()) {
        o.insert(QStringLiteral("maxQuantum"), static_cast<qint64>(*s.maxQuantum));
      } else {
        o.insert(QStringLiteral("maxQuantum"), QJsonValue());
      }
      if (s.forceRate.has_value()) {
        o.insert(QStringLiteral("forceRate"), static_cast<qint64>(*s.forceRate));
      } else {
        o.insert(QStringLiteral("forceRate"), QJsonValue());
      }
      if (s.forceQuantum.has_value()) {
        o.insert(QStringLiteral("forceQuantum"), static_cast<qint64>(*s.forceQuantum));
      } else {
        o.insert(QStringLiteral("forceQuantum"), QJsonValue());
      }
      QJsonArray arr;
      for (uint32_t r : s.allowedRates) {
        arr.append(static_cast<qint64>(r));
      }
      o.insert(QStringLiteral("allowedRates"), arr);
      return o;
    };

    if (clockSub == QStringLiteral("presets")) {
      const auto presets = PipeWireGraph::clockPresets();
      if (jsonOutput) {
        QJsonArray arr;
        for (const auto& p : presets) {
          QJsonObject o;
          o.insert(QStringLiteral("id"), p.id);
          o.insert(QStringLiteral("title"), p.title);
          if (p.forceRate.has_value()) {
            o.insert(QStringLiteral("forceRate"), static_cast<qint64>(*p.forceRate));
          } else {
            o.insert(QStringLiteral("forceRate"), QJsonValue());
          }
          if (p.forceQuantum.has_value()) {
            o.insert(QStringLiteral("forceQuantum"), static_cast<qint64>(*p.forceQuantum));
          } else {
            o.insert(QStringLiteral("forceQuantum"), QJsonValue());
          }
          arr.append(o);
        }
        out << QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        for (const auto& p : presets) {
          out << p.id << "\t" << p.title << "\n";
        }
      }
      exitCode = 0;
      break;
    }

    if (!graph.hasClockSettingsSupport()) {
      err << "headroomctl: PipeWire clock controls unavailable (settings metadata missing)\n";
      exitCode = 1;
      break;
    }

    if (clockSub == QStringLiteral("preset")) {
      if (args.size() < 5) {
        err << "headroomctl: engine clock preset expects <preset-id>\n";
        exitCode = 2;
        break;
      }
      const QString presetId = args.at(4).trimmed();
      const bool ok = graph.applyClockPreset(presetId);
      if (!ok) {
        err << "headroomctl: failed to apply preset: " << presetId << "\n";
        exitCode = 1;
        break;
      }

      waitForGraph(120);
      const PwClockSettings s = graph.clockSettings();
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("preset"), presetId);
        o.insert(QStringLiteral("clock"), settingsToJson(s));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        out << "ok\tpreset=" << presetId << "\n";
      }
      exitCode = 0;
      break;
    }

    if (clockSub == QStringLiteral("set")) {
      bool rateProvided = false;
      bool quantumProvided = false;
      bool minProvided = false;
      bool maxProvided = false;
      std::optional<uint32_t> rate;
      std::optional<uint32_t> quantum;
      std::optional<uint32_t> minQ;
      std::optional<uint32_t> maxQ;

      auto parseMaybeU32 = [](const QString& s, bool* okOut) -> std::optional<uint32_t> {
        const QString t = s.trimmed().toLower();
        if (t == QStringLiteral("auto") || t == QStringLiteral("off")) {
          if (okOut) {
            *okOut = true;
          }
          return std::nullopt;
        }
        bool ok = false;
        const uint32_t v = t.toUInt(&ok);
        if (okOut) {
          *okOut = ok;
        }
        return ok ? std::optional<uint32_t>(v) : std::nullopt;
      };

      for (int i = 4; i < args.size(); ++i) {
        const QString a = args.at(i);
        auto takeValue = [&](QString* outValue) -> bool {
          const int eq = a.indexOf('=');
          if (eq >= 0) {
            *outValue = a.mid(eq + 1);
            return true;
          }
          if (i + 1 >= args.size()) {
            return false;
          }
          *outValue = args.at(++i);
          return true;
        };

        QString v;
        if (a == QStringLiteral("--rate") || a.startsWith(QStringLiteral("--rate="))) {
          if (!takeValue(&v)) {
            err << "headroomctl: --rate expects a value\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          bool ok = false;
          rate = parseMaybeU32(v, &ok);
          if (!ok) {
            err << "headroomctl: invalid --rate value: " << v << "\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          rateProvided = true;
          continue;
        }

        if (a == QStringLiteral("--quantum") || a.startsWith(QStringLiteral("--quantum="))) {
          if (!takeValue(&v)) {
            err << "headroomctl: --quantum expects a value\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          bool ok = false;
          quantum = parseMaybeU32(v, &ok);
          if (!ok) {
            err << "headroomctl: invalid --quantum value: " << v << "\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          quantumProvided = true;
          continue;
        }

        if (a == QStringLiteral("--min-quantum") || a.startsWith(QStringLiteral("--min-quantum="))) {
          if (!takeValue(&v)) {
            err << "headroomctl: --min-quantum expects a value\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          bool ok = false;
          minQ = parseMaybeU32(v, &ok);
          if (!ok) {
            err << "headroomctl: invalid --min-quantum value: " << v << "\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          minProvided = true;
          continue;
        }

        if (a == QStringLiteral("--max-quantum") || a.startsWith(QStringLiteral("--max-quantum="))) {
          if (!takeValue(&v)) {
            err << "headroomctl: --max-quantum expects a value\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          bool ok = false;
          maxQ = parseMaybeU32(v, &ok);
          if (!ok) {
            err << "headroomctl: invalid --max-quantum value: " << v << "\n";
            exitCode = 2;
            goto engine_clock_set_done;
          }
          maxProvided = true;
          continue;
        }

        err << "headroomctl: unknown option: " << a << "\n";
        exitCode = 2;
        goto engine_clock_set_done;
      }

      {
        bool okAll = true;
        if (rateProvided) {
          okAll = okAll && graph.setClockForceRate(rate);
        }
        if (quantumProvided) {
          okAll = okAll && graph.setClockForceQuantum(quantum);
        }
        if (minProvided) {
          okAll = okAll && graph.setClockMinQuantum(minQ);
        }
        if (maxProvided) {
          okAll = okAll && graph.setClockMaxQuantum(maxQ);
        }
        waitForGraph(120);
        const PwClockSettings s = graph.clockSettings();

        if (jsonOutput) {
          QJsonObject o;
          o.insert(QStringLiteral("ok"), okAll);
          QJsonObject applied;
          if (rateProvided) {
            applied.insert(QStringLiteral("rate"), rate.has_value() ? QJsonValue(static_cast<qint64>(*rate)) : QJsonValue());
          }
          if (quantumProvided) {
            applied.insert(QStringLiteral("quantum"), quantum.has_value() ? QJsonValue(static_cast<qint64>(*quantum)) : QJsonValue());
          }
          if (minProvided) {
            applied.insert(QStringLiteral("minQuantum"), minQ.has_value() ? QJsonValue(static_cast<qint64>(*minQ)) : QJsonValue());
          }
          if (maxProvided) {
            applied.insert(QStringLiteral("maxQuantum"), maxQ.has_value() ? QJsonValue(static_cast<qint64>(*maxQ)) : QJsonValue());
          }
          o.insert(QStringLiteral("applied"), applied);
          o.insert(QStringLiteral("clock"), settingsToJson(s));
          out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
        } else {
          out << (okAll ? "ok" : "failed") << "\n";
        }

        exitCode = okAll ? 0 : 1;
      }

    engine_clock_set_done:
      break;
    }

    // Default: status
    if (clockSub == QStringLiteral("status")) {
      const PwClockSettings s = graph.clockSettings();
      if (jsonOutput) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("clock"), settingsToJson(s));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
      } else {
        const uint32_t rateNow = s.rate.value_or(0);
        const uint32_t quantumNow = s.quantum.value_or(0);
        out << "rate\t" << (rateNow > 0 ? QString::number(rateNow) : QStringLiteral("-")) << "\n";
        out << "quantum\t" << (quantumNow > 0 ? QString::number(quantumNow) : QStringLiteral("-")) << "\n";
        out << "force-rate\t" << (s.forceRate.has_value() ? QString::number(*s.forceRate) : QStringLiteral("auto")) << "\n";
        out << "force-quantum\t" << (s.forceQuantum.has_value() ? QString::number(*s.forceQuantum) : QStringLiteral("auto")) << "\n";
        out << "min-quantum\t" << (s.minQuantum.has_value() ? QString::number(*s.minQuantum) : QStringLiteral("-")) << "\n";
        out << "max-quantum\t" << (s.maxQuantum.has_value() ? QString::number(*s.maxQuantum) : QStringLiteral("-")) << "\n";
      }
      exitCode = 0;
      break;
    }

    err << "headroomctl: engine clock expects status|presets|preset|set\n";
    exitCode = 2;
    break;
  } while (false);

  return exitCode;
}
} // namespace headroomctl
