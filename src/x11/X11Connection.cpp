#include "X11Connection.hpp"
#include "X11ConnectionPrivate.hpp"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

using GeometryReply = std::unique_ptr<xcb_get_geometry_reply_t, ReplyDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, ReplyDeleter>;

[[nodiscard]] std::uint64_t nextConnectionIdentity() noexcept {
    static std::atomic_uint64_t next{1U};
    std::uint64_t identity = next.fetch_add(1U);
    while (identity == 0U) {
        identity = next.fetch_add(1U);
    }
    return identity;
}

[[nodiscard]] bool validDisplayName(std::string_view display) noexcept {
    if (display.empty() || display.size() > maximumDisplayNameBytes ||
        display.find('\0') != std::string_view::npos) {
        return false;
    }
    for (const unsigned char character : display) {
        if (character <= 0x20U || character == 0x7fU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<X11Connection> unavailable() {
    return Result<X11Connection>::failure(
        {ErrorCode::invalid_environment, "The selected X11 display is unavailable.",
         "Start the development X server and provide its bounded DISPLAY value."});
}

} // namespace

Result<X11Connection> X11Connection::connect(std::string_view display) {
    if (!validDisplayName(display)) {
        return unavailable();
    }

    int screenIndex = 0;
    const std::string displayName{display};
    detail::ConnectionHandle connection{xcb_connect(displayName.c_str(), &screenIndex)};
    if (!connection || screenIndex < 0 || xcb_connection_has_error(connection.get()) != 0) {
        return unavailable();
    }

    const xcb_setup_t *setup = xcb_get_setup(connection.get());
    if (setup == nullptr) {
        return unavailable();
    }
    auto iterator = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
        xcb_screen_next(&iterator);
    }
    if (iterator.rem <= 0 || iterator.data == nullptr || iterator.data->root == XCB_WINDOW_NONE) {
        return unavailable();
    }

    auto root = WindowId::fromProtocol(iterator.data->root);
    if (!root) {
        return unavailable();
    }
    xcb_generic_error_t *rawProtocolError = nullptr;
    GeometryReply geometry{xcb_get_geometry_reply(
        connection.get(), xcb_get_geometry(connection.get(), iterator.data->root),
        &rawProtocolError)};
    ProtocolError protocolError{rawProtocolError};
    if (!geometry || protocolError || xcb_connection_has_error(connection.get()) != 0 ||
        geometry->width == 0U || geometry->height == 0U || geometry->root != iterator.data->root) {
        return unavailable();
    }

    const ScreenInfo screen{static_cast<std::uint32_t>(screenIndex), root.value(), geometry->width,
                            geometry->height};
    return Result<X11Connection>::success(X11Connection{
        std::make_shared<Implementation>(std::move(connection), screen, nextConnectionIdentity())});
}

X11Connection::X11Connection(std::shared_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

X11Connection::~X11Connection() = default;
X11Connection::X11Connection(X11Connection &&) noexcept = default;
X11Connection &X11Connection::operator=(X11Connection &&) noexcept = default;

const ScreenInfo &X11Connection::screen() const noexcept { return implementation_->screen_; }

int X11Connection::eventFileDescriptor() const noexcept {
    return xcb_get_file_descriptor(implementation_->connection_.get());
}

bool X11Connection::healthy() const noexcept {
    return implementation_ && implementation_->connection_ &&
           xcb_connection_has_error(implementation_->connection_.get()) == 0;
}

void *X11Connection::nativeConnection() const noexcept {
    return implementation_ != nullptr ? implementation_->connection_.get() : nullptr;
}

X11Connection::Identity X11Connection::identity() const noexcept {
    return implementation_ != nullptr ? implementation_->identity_ : 0U;
}

Result<ScreenInfo> probeUsableDisplay(std::string_view display) {
    auto connection = X11Connection::connect(display);
    if (!connection) {
        return Result<ScreenInfo>::failure(connection.error());
    }
    return Result<ScreenInfo>::success(connection.value().screen());
}

} // namespace prismdrake::x11
