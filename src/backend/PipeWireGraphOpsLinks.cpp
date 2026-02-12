#include "PipeWireGraphInternal.h"
#include "DebugEnv.h"

#include <QDebug>

#include <pipewire/core.h>
#include <pipewire/link.h>
#include <pipewire/properties.h>

namespace {
QString nodeLabel(const PwNodeInfo& n)
{
  return n.description.isEmpty() ? n.name : n.description;
}
} // namespace

bool PipeWireGraph::createLink(uint32_t outputNodeId, uint32_t outputPortId, uint32_t inputNodeId, uint32_t inputPortId)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  PwNodeInfo outNode{};
  PwNodeInfo inNode{};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    outNode = m_nodes.value(outputNodeId, PwNodeInfo{});
    inNode = m_nodes.value(inputNodeId, PwNodeInfo{});
  }

  const bool debugOps = headroom::debug::pwOpsEnabled() &&
      (headroom::debug::pwNodeFilterMatches(outputNodeId, outNode.name, outNode.description) ||
       headroom::debug::pwNodeFilterMatches(inputNodeId, inNode.name, inNode.description));
  auto logOp = [&](const QString& msg) {
    qDebug().noquote() << QStringLiteral("headroom: debug: %1").arg(msg);
  };

  if (!m_registry) {
    if (debugOps) {
      logOp(QStringLiteral("createLink outNode=%1(%2) outPort=%3 inNode=%4(%5) inPort=%6 (missing registry)")
                .arg(nodeLabel(outNode))
                .arg(outputNodeId)
                .arg(outputPortId)
                .arg(nodeLabel(inNode))
                .arg(inputNodeId)
                .arg(inputPortId));
    }
    pw_thread_loop_unlock(loop);
    return false;
  }

  if (debugOps) {
    logOp(QStringLiteral("createLink outNode=%1(%2) outPort=%3 inNode=%4(%5) inPort=%6")
              .arg(nodeLabel(outNode))
              .arg(outputNodeId)
              .arg(outputPortId)
              .arg(nodeLabel(inNode))
              .arg(inputNodeId)
              .arg(inputPortId));
  }

  pw_properties* props = pw_properties_new(nullptr, nullptr);
  pw_properties_set(props, PW_KEY_OBJECT_LINGER, "true");
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", outputNodeId);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", outputPortId);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", inputNodeId);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", inputPortId);

  void* proxy = pw_core_create_object(m_pw->core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
  pw_properties_free(props);

  if (proxy) {
    // Keep the proxy alive for the lifetime of this PipeWire connection.
    // This avoids destroying the server-side resource (the link). With
    // object.linger=true, links can outlive this process when we disconnect.
    m_createdLinkProxies.push_back(proxy);
  }

  if (debugOps) {
    logOp(QStringLiteral("createLink ok=%1 proxy=%2").arg(proxy != nullptr ? 1 : 0).arg(proxy != nullptr ? QStringLiteral("yes") : QStringLiteral("no")));
  }

  pw_thread_loop_unlock(loop);
  return proxy != nullptr;
}

bool PipeWireGraph::destroyLink(uint32_t linkId)
{
  if (!m_pw || !m_pw->isConnected() || !m_pw->threadLoop()) {
    return false;
  }

  pw_thread_loop* loop = m_pw->threadLoop();
  pw_thread_loop_lock(loop);

  if (!m_registry) {
    pw_thread_loop_unlock(loop);
    return false;
  }

  const int res = pw_registry_destroy(m_registry, linkId);
  if (headroom::debug::pwOpsEnabled()) {
    const QString msg = QStringLiteral("destroyLink linkId=%1 res=%2").arg(linkId).arg(res);
    qDebug().noquote() << QStringLiteral("headroom: debug: %1").arg(msg);
  }
  pw_thread_loop_unlock(loop);
  return res >= 0;
}
