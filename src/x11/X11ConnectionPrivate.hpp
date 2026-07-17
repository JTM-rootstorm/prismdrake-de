#pragma once

#include "X11Connection.hpp"

#include <memory>
#include <utility>

#include <xcb/xcb.h>

namespace prismdrake::x11::detail {

struct ConnectionDeleter final {
    void operator()(xcb_connection_t *connection) const noexcept {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
    }
};

using ConnectionHandle = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;

} // namespace prismdrake::x11::detail

namespace prismdrake::x11 {

/// Shared opaque transport state used only by trusted X11 adapters.
class X11Connection::Implementation final {
  public:
    Implementation(detail::ConnectionHandle connection, ScreenInfo screen,
                   std::uint64_t identity) noexcept
        : connection_(std::move(connection)), screen_(screen), identity_(identity) {}

    detail::ConnectionHandle connection_;
    ScreenInfo screen_;
    std::uint64_t identity_;
};

} // namespace prismdrake::x11
