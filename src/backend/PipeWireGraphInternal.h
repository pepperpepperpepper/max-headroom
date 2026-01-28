#pragma once

#include "PipeWireGraph.h"

#include "PipeWireThread.h"

#include <pipewire/keys.h>

#include <spa/utils/dict.h>

struct pw_node;
struct pw_metadata;
struct pw_profiler;

namespace pipewiregraph_internal {
inline QString dictString(const spa_dict* dict, const char* key)
{
  if (!dict || !key) {
    return {};
  }
  if (const char* value = spa_dict_lookup(dict, key)) {
    return QString::fromUtf8(value);
  }
  return {};
}

inline std::optional<uint32_t> dictU32(const spa_dict* dict, const char* key)
{
  if (!dict || !key) {
    return std::nullopt;
  }
  const char* value = spa_dict_lookup(dict, key);
  if (!value) {
    return std::nullopt;
  }
  bool ok = false;
  const uint32_t parsed = QString::fromUtf8(value).toUInt(&ok);
  return ok ? std::optional<uint32_t>(parsed) : std::nullopt;
}
} // namespace pipewiregraph_internal

struct PipeWireGraph::NodeBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t nodeId = 0;
  pw_node* node = nullptr;
  spa_hook listener{};
};

struct PipeWireGraph::MetadataBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t metadataId = 0;
  QString name;
  pw_metadata* metadata = nullptr;
  spa_hook listener{};
};

struct PipeWireGraph::ProfilerBinding final {
  PipeWireGraph* graph = nullptr;
  uint32_t profilerId = 0;
  pw_profiler* profiler = nullptr;
  spa_hook listener{};
};

