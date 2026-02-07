#include "PipeWireGraphInternal.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

int PipeWireGraph::onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value)
{
  auto* binding = static_cast<MetadataBinding*>(data);
  if (!binding || !binding->graph || !key) {
    return 0;
  }

  PipeWireGraph* graph = binding->graph;
  if (subject != 0) {
    return 0;
  }

  const QString k = QString::fromUtf8(key);
  auto parseU32 = [](const char* s) -> std::optional<uint32_t> {
    if (!s || s[0] == '\0') {
      return std::nullopt;
    }
    bool ok = false;
    const uint32_t v = QString::fromUtf8(s).toUInt(&ok);
    return ok ? std::optional<uint32_t>(v) : std::nullopt;
  };

  auto parseAllowedRates = [](const QString& raw) -> QVector<uint32_t> {
    QString s = raw;
    s.remove('[');
    s.remove(']');
    const QStringList parts = s.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QVector<uint32_t> out;
    out.reserve(parts.size());
    for (const auto& p : parts) {
      bool ok = false;
      const uint32_t v = p.toUInt(&ok);
      if (ok) {
        out.push_back(v);
      }
    }
    return out;
  };

  const QByteArray t = QByteArray(type ? type : "");
  const bool valueLooksJson = (value && value[0] == '{');
  const bool isJson = t.contains("JSON") || valueLooksJson;

  auto parseNameFromJson = [](const char* raw) -> QString {
    if (!raw || raw[0] == '\0') {
      return {};
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(raw), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
      return {};
    }
    const QJsonObject o = doc.object();
    return o.value(QStringLiteral("name")).toString();
  };

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(graph->m_mutex);
    auto updateU32 = [&](std::optional<uint32_t>& slot) {
      const std::optional<uint32_t> parsed = parseU32(value);
      if (slot != parsed) {
        slot = parsed;
        changed = true;
      }
    };

    auto updateDefaultDevice = [&](std::optional<uint32_t>& idSlot, QString& nameSlot) {
      if (isJson) {
        graph->m_defaultDevicesPreferJson = true;
        const QString parsedName = parseNameFromJson(value);
        if (nameSlot != parsedName) {
          nameSlot = parsedName;
          changed = true;
        }
        if (idSlot.has_value()) {
          idSlot.reset();
          changed = true;
        }
        return;
      }

      const std::optional<uint32_t> parsed = parseU32(value);
      if (idSlot != parsed) {
        idSlot = parsed;
        changed = true;
      }
      if (!nameSlot.isEmpty()) {
        nameSlot.clear();
        changed = true;
      }
    };

    if (k == QStringLiteral("default.audio.sink")) {
      updateDefaultDevice(graph->m_defaultAudioSinkId, graph->m_defaultAudioSinkName);
    } else if (k == QStringLiteral("default.configured.audio.sink")) {
      updateDefaultDevice(graph->m_configuredAudioSinkId, graph->m_configuredAudioSinkName);
    } else if (k == QStringLiteral("default.audio.source")) {
      updateDefaultDevice(graph->m_defaultAudioSourceId, graph->m_defaultAudioSourceName);
    } else if (k == QStringLiteral("default.configured.audio.source")) {
      updateDefaultDevice(graph->m_configuredAudioSourceId, graph->m_configuredAudioSourceName);
    } else if (binding->name == QStringLiteral("settings")) {
      if (k == QStringLiteral("clock.rate")) {
        updateU32(graph->m_clockRate);
      } else if (k == QStringLiteral("clock.allowed-rates")) {
        const QString raw = QString::fromUtf8(value ? value : "");
        const QVector<uint32_t> parsed = parseAllowedRates(raw);
        if (graph->m_clockAllowedRates != parsed) {
          graph->m_clockAllowedRates = parsed;
          changed = true;
        }
      } else if (k == QStringLiteral("clock.quantum")) {
        updateU32(graph->m_clockQuantum);
      } else if (k == QStringLiteral("clock.min-quantum")) {
        updateU32(graph->m_clockMinQuantum);
      } else if (k == QStringLiteral("clock.max-quantum")) {
        updateU32(graph->m_clockMaxQuantum);
      } else if (k == QStringLiteral("clock.force-rate")) {
        const std::optional<uint32_t> v = parseU32(value);
        const std::optional<uint32_t> parsed = (v.has_value() && *v > 0) ? v : std::nullopt;
        if (graph->m_clockForceRate != parsed) {
          graph->m_clockForceRate = parsed;
          changed = true;
        }
      } else if (k == QStringLiteral("clock.force-quantum")) {
        const std::optional<uint32_t> v = parseU32(value);
        const std::optional<uint32_t> parsed = (v.has_value() && *v > 0) ? v : std::nullopt;
        if (graph->m_clockForceQuantum != parsed) {
          graph->m_clockForceQuantum = parsed;
          changed = true;
        }
      }
    }
  }

  if (changed) {
    graph->scheduleGraphChanged(ChangeMetadata);
  }

  return 0;
}
