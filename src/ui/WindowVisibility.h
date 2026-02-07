#pragma once

#include <QtGlobal>

#include <QGuiApplication>
#include <QWidget>
#include <QTabWidget>

#include <cstdlib>
#include <optional>

#if QT_CONFIG(xcb)
#include <QtGui/qguiapplication_platform.h>

#include <xcb/xcb.h>
#endif

namespace WindowVisibility {
namespace detail {
inline std::optional<bool> x11WindowViewableMaybe(const QWidget* window)
{
#if QT_CONFIG(xcb)
  if (!window) {
    return std::nullopt;
  }

  const WId id = window->winId();
  if (id == 0) {
    return std::nullopt;
  }

  auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
  if (!x11) {
    return std::nullopt;
  }
  xcb_connection_t* conn = x11->connection();
  if (!conn) {
    return std::nullopt;
  }

  const xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes(conn, static_cast<xcb_window_t>(id));
  xcb_get_window_attributes_reply_t* reply = xcb_get_window_attributes_reply(conn, cookie, nullptr);
  if (!reply) {
    return std::nullopt;
  }

  const bool viewable = (reply->map_state == XCB_MAP_STATE_VIEWABLE);
  std::free(reply);
  return viewable;
#else
  Q_UNUSED(window);
  return std::nullopt;
#endif
}

inline bool requestX11Map(const QWidget* window)
{
#if QT_CONFIG(xcb)
  if (!window) {
    return false;
  }

  const WId id = window->winId();
  if (id == 0) {
    return false;
  }

  auto* x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
  if (!x11) {
    return false;
  }
  xcb_connection_t* conn = x11->connection();
  if (!conn) {
    return false;
  }

  xcb_map_window(conn, static_cast<xcb_window_t>(id));
  xcb_flush(conn);
  return true;
#else
  Q_UNUSED(window);
  return false;
#endif
}
} // namespace detail

inline bool isActive(const QWidget* window)
{
  if (!window) {
    return false;
  }

  if (window->windowState() & Qt::WindowMinimized) {
    return false;
  }

  if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
    // Some real-host X11 environments can map/unmap the native window without Qt updating
    // isVisible()/WA_Mapped or windowState() in lock-step. Prefer the X server map-state
    // when available so low-power gating follows the *actual* on-screen state.
    if (const auto viewable = detail::x11WindowViewableMaybe(window); viewable.has_value()) {
      return *viewable;
    }

    return window->isVisible() && window->testAttribute(Qt::WA_Mapped);
  }

  return window->isVisible();
}

inline bool requestMap(const QWidget* window)
{
  if (!window) {
    return false;
  }
  if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
    return false;
  }
  return detail::requestX11Map(window);
}

inline bool isInActiveTab(const QWidget* widget)
{
  if (!widget) {
    return false;
  }

  for (const QWidget* p = widget; p; p = p->parentWidget()) {
    if (const auto* tabs = qobject_cast<const QTabWidget*>(p)) {
      const QWidget* page = tabs->currentWidget();
      if (!page) {
        return false;
      }
      return page == widget || page->isAncestorOf(widget);
    }
  }

  return true;
}
} // namespace WindowVisibility
