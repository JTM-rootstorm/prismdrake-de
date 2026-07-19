#include "DesktopProperties.hpp"

#include "AtomCache.hpp"
#include "X11Connection.hpp"

#include <cstdlib>
#include <memory>
#include <optional>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;
using WindowAttributes = std::unique_ptr<xcb_get_window_attributes_reply_t, FreeDeleter>;

struct PublicationContext final {
    xcb_connection_t *connection;
    WindowId window;
    AtomId atom;
    AtomId windowType;
    AtomId desktopType;
};

template <typename Value> [[nodiscard]] Result<Value> invalidContext() {
    return Result<Value>::failure({ErrorCode::invalid_argument,
                                   "The X11 desktop publication context is invalid.",
                                   "Use an atom cache created from the same live X11 connection."});
}

template <typename Value> [[nodiscard]] Result<Value> invalidTarget() {
    return Result<Value>::failure(
        {ErrorCode::invalid_argument, "The X11 desktop publication target is invalid.",
         "Use a non-root live desktop window on the selected X11 connection."});
}

template <typename Value> [[nodiscard]] Result<Value> transportFailure() {
    return Result<Value>::failure(
        {ErrorCode::io_error, "The X11 desktop property operation failed.",
         "Discard the publication result and rebuild desktop state from a live X11 connection."});
}

[[nodiscard]] std::optional<PublicationContext>
contextFor(xcb_connection_t *native, const AtomCache &atoms, WindowId window) {
    const auto atom = atoms.atom(AtomName::atom);
    const auto windowType = atoms.atom(AtomName::net_wm_window_type);
    const auto desktopType = atoms.atom(AtomName::net_wm_window_type_desktop);
    if (native == nullptr || !atom || !windowType || !desktopType) {
        return std::nullopt;
    }
    return PublicationContext{native, window, *atom, *windowType, *desktopType};
}

[[nodiscard]] bool targetExists(const PublicationContext &context) {
    xcb_generic_error_t *rawError = nullptr;
    const auto cookie = xcb_get_window_attributes(context.connection, context.window.value());
    WindowAttributes attributes{
        xcb_get_window_attributes_reply(context.connection, cookie, &rawError)};
    ProtocolError error{rawError};
    return !error && attributes && xcb_connection_has_error(context.connection) == 0;
}

[[nodiscard]] Result<PublicationContext>
prepare(xcb_connection_t *native, std::uint64_t connectionIdentity, bool connectionHealthy,
        bool atomsBelong, WindowId root, const AtomCache &atoms, WindowId window) {
    if (connectionIdentity == 0U || !atomsBelong) {
        return invalidContext<PublicationContext>();
    }
    if (window == root) {
        return invalidTarget<PublicationContext>();
    }
    const auto context = contextFor(native, atoms, window);
    if (!connectionHealthy || !context) {
        return transportFailure<PublicationContext>();
    }
    return Result<PublicationContext>::success(*context);
}

} // namespace

Result<void> DesktopProperties::publish(X11Connection &connection, const AtomCache &atoms,
                                        WindowId window) {
    const auto identity = connection.identity();
    auto context = prepare(static_cast<xcb_connection_t *>(connection.nativeConnection()), identity,
                           connection.healthy(), atoms.belongsTo(identity),
                           connection.screen().rootWindow, atoms, window);
    if (!context) {
        return Result<void>::failure(context.error());
    }
    if (!targetExists(context.value())) {
        return transportFailure<void>();
    }
    const auto value = context.value().desktopType.value();
    const auto cookie = xcb_change_property_checked(
        context.value().connection, XCB_PROP_MODE_REPLACE, window.value(),
        context.value().windowType.value(), context.value().atom.value(), 32U, 1U, &value);
    ProtocolError error{xcb_request_check(context.value().connection, cookie)};
    if (error || xcb_connection_has_error(context.value().connection) != 0) {
        return transportFailure<void>();
    }
    return Result<void>::success();
}

Result<void> DesktopProperties::remove(X11Connection &connection, const AtomCache &atoms,
                                       WindowId window) {
    const auto identity = connection.identity();
    auto context = prepare(static_cast<xcb_connection_t *>(connection.nativeConnection()), identity,
                           connection.healthy(), atoms.belongsTo(identity),
                           connection.screen().rootWindow, atoms, window);
    if (!context) {
        return Result<void>::failure(context.error());
    }
    const auto cookie = xcb_delete_property_checked(context.value().connection, window.value(),
                                                    context.value().windowType.value());
    ProtocolError error{xcb_request_check(context.value().connection, cookie)};
    if (error && error->error_code != XCB_WINDOW) {
        return transportFailure<void>();
    }
    if (xcb_connection_has_error(context.value().connection) != 0) {
        return transportFailure<void>();
    }
    return Result<void>::success();
}

} // namespace prismdrake::x11
