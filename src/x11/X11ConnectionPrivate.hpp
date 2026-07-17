#pragma once

#include "X11Connection.hpp"

#include <atomic>
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

    [[nodiscard]] bool tryAcquireRootEventStream() noexcept {
        bool expected = false;
        return rootEventStreamActive_.compare_exchange_strong(expected, true);
    }

    void releaseRootEventStream() noexcept { rootEventStreamActive_.store(false); }

    detail::ConnectionHandle connection_;
    ScreenInfo screen_;
    std::uint64_t identity_;

  private:
    std::atomic_bool rootEventStreamActive_{false};
};

} // namespace prismdrake::x11
