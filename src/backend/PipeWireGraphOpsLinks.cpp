#include "PipeWireGraphInternal.h"

#include <pipewire/core.h>
#include <pipewire/link.h>
#include <pipewire/properties.h>

bool PipeWireGraph::createLink(uint32_t outputNodeId, uint32_t outputPortId, uint32_t inputNodeId, uint32_t inputPortId)
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
  pw_thread_loop_unlock(loop);
  return res >= 0;
}

