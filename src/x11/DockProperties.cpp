#include "DockProperties.hpp"

#include "AtomCache.hpp"
#include "DockPropertiesInternal.hpp"
#include "X11Connection.hpp"

#include <array>
#include <cstdlib>
#include <limits>
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

struct PublicationAtoms final {
    AtomId atom;
    AtomId cardinal;
    AtomId windowType;
    AtomId dockWindowType;
    AtomId strut;
    AtomId strutPartial;
};

struct PublicationContext final {
    xcb_connection_t *connection;
    WindowId window;
    PublicationAtoms atoms;
};

[[nodiscard]] Result<void> invalidTarget() {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The X11 dock publication target is invalid.",
         "Use a non-root live panel window on the selected X11 connection."});
}

template <typename Value> [[nodiscard]] Result<Value> invalidContext() {
    return Result<Value>::failure({ErrorCode::invalid_argument,
                                   "The X11 dock publication context is invalid.",
                                   "Use an atom cache created from the same live X11 connection."});
}

template <typename Value> [[nodiscard]] Result<Value> transportFailure() {
    return Result<Value>::failure(
        {ErrorCode::io_error, "The X11 dock property operation failed.",
         "Discard the publication result and rebuild panel state from a live X11 connection."});
}

[[nodiscard]] std::optional<PublicationAtoms> publicationAtoms(const AtomCache &atoms) {
    const auto atom = atoms.atom(AtomName::atom);
    const auto cardinal = atoms.atom(AtomName::cardinal);
    const auto windowType = atoms.atom(AtomName::net_wm_window_type);
    const auto dockWindowType = atoms.atom(AtomName::net_wm_window_type_dock);
    const auto strut = atoms.atom(AtomName::net_wm_strut);
    const auto strutPartial = atoms.atom(AtomName::net_wm_strut_partial);
    if (!atom || !cardinal || !windowType || !dockWindowType || !strut || !strutPartial) {
        return std::nullopt;
    }
    return PublicationAtoms{*atom, *cardinal, *windowType, *dockWindowType, *strut, *strutPartial};
}

[[nodiscard]] Result<PublicationContext>
prepareContext(xcb_connection_t *native, std::uint64_t connectionIdentity, bool connectionHealthy,
               bool atomsBelong, WindowId root, const AtomCache &atoms, WindowId window) {
    if (connectionIdentity == 0U || !atomsBelong) {
        return invalidContext<PublicationContext>();
    }
    auto target = detail::validateDockTarget(window, root);
    if (!target) {
        return Result<PublicationContext>::failure(target.error());
    }
    const auto requiredAtoms = publicationAtoms(atoms);
    if (native == nullptr || !connectionHealthy || !requiredAtoms) {
        return transportFailure<PublicationContext>();
    }
    return Result<PublicationContext>::success(
        PublicationContext{native, window, requiredAtoms.value()});
}

[[nodiscard]] bool targetExists(const PublicationContext &context) {
    xcb_generic_error_t *rawError = nullptr;
    const auto cookie = xcb_get_window_attributes(context.connection, context.window.value());
    WindowAttributes attributes{
        xcb_get_window_attributes_reply(context.connection, cookie, &rawError)};
    ProtocolError error{rawError};
    return !error && attributes && xcb_connection_has_error(context.connection) == 0;
}

[[nodiscard]] Result<void> removeWithContext(const PublicationContext &context) {
    const std::array cookies{
        xcb_delete_property_checked(context.connection, context.window.value(),
                                    context.atoms.windowType.value()),
        xcb_delete_property_checked(context.connection, context.window.value(),
                                    context.atoms.strut.value()),
        xcb_delete_property_checked(context.connection, context.window.value(),
                                    context.atoms.strutPartial.value()),
    };

    bool failed = false;
    for (const auto cookie : cookies) {
        ProtocolError error{xcb_request_check(context.connection, cookie)};
        if (error && error->error_code != XCB_WINDOW) {
            failed = true;
        }
    }
    if (failed || xcb_connection_has_error(context.connection) != 0) {
        return transportFailure<void>();
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> publishWithContext(const PublicationContext &context,
                                              const detail::DockPropertyPayload &payload) {
    if (!targetExists(context)) {
        return transportFailure<void>();
    }

    const std::array cookies{
        xcb_change_property_checked(
            context.connection, XCB_PROP_MODE_REPLACE, context.window.value(),
            context.atoms.windowType.value(), context.atoms.atom.value(), 32U,
            static_cast<std::uint32_t>(payload.windowType.size()), payload.windowType.data()),
        xcb_change_property_checked(
            context.connection, XCB_PROP_MODE_REPLACE, context.window.value(),
            context.atoms.strut.value(), context.atoms.cardinal.value(), 32U,
            static_cast<std::uint32_t>(payload.strut.size()), payload.strut.data()),
        xcb_change_property_checked(
            context.connection, XCB_PROP_MODE_REPLACE, context.window.value(),
            context.atoms.strutPartial.value(), context.atoms.cardinal.value(), 32U,
            static_cast<std::uint32_t>(payload.strutPartial.size()), payload.strutPartial.data()),
    };

    bool failed = false;
    for (const auto cookie : cookies) {
        ProtocolError error{xcb_request_check(context.connection, cookie)};
        failed = failed || static_cast<bool>(error);
    }
    if (failed || xcb_connection_has_error(context.connection) != 0) {
        const auto rollback = removeWithContext(context);
        (void)rollback;
        return transportFailure<void>();
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> publishReservation(const PublicationContext &context,
                                              const BottomPanelStrut &reservation) {
    auto payload = detail::makeDockPropertyPayload(context.atoms.dockWindowType, reservation);
    if (!payload) {
        auto removed = removeWithContext(context);
        if (!removed) {
            return removed;
        }
        return Result<void>::failure(payload.error());
    }
    return publishWithContext(context, payload.value());
}

} // namespace

namespace detail {

Result<DockPropertyPayload> makeDockPropertyPayload(AtomId dockWindowType,
                                                    const BottomPanelStrut &reservation) {
    const auto &panel = reservation.panel;
    const auto right = static_cast<std::uint64_t>(panel.x) + panel.width;
    const auto panelBottom = static_cast<std::uint64_t>(panel.y) + panel.height;
    const auto inferredRootHeight = static_cast<std::uint64_t>(panel.y) + reservation.strut[3];
    const std::array expectedStrut{0U, 0U, 0U, reservation.strut[3]};
    const std::array expectedPartial{
        0U, 0U, 0U,      reservation.strut[3],
        0U, 0U, 0U,      0U,
        0U, 0U, panel.x, right == 0U ? 0U : static_cast<std::uint32_t>(right - 1U)};
    if (panel.width == 0U || panel.height == 0U || right == 0U ||
        right > std::numeric_limits<std::uint32_t>::max() ||
        panelBottom > std::numeric_limits<std::uint32_t>::max() ||
        inferredRootHeight > std::numeric_limits<std::uint32_t>::max() ||
        reservation.strut[3] < panel.height || reservation.strut != expectedStrut ||
        reservation.strutPartial != expectedPartial) {
        return Result<DockPropertyPayload>::failure(
            {ErrorCode::validation_error, "The X11 dock reservation is invalid.",
             "Recalculate the complete reservation from validated root and output geometry."});
    }
    return Result<DockPropertyPayload>::success(
        DockPropertyPayload{{dockWindowType.value()}, reservation.strut, reservation.strutPartial});
}

Result<void> validateDockTarget(WindowId window, WindowId root) {
    if (window == root) {
        return invalidTarget();
    }
    return Result<void>::success();
}

} // namespace detail

Result<BottomPanelStrut> DockProperties::publishBottomPanel(X11Connection &connection,
                                                            const AtomCache &atoms, WindowId window,
                                                            RootGeometry root,
                                                            OutputGeometry output,
                                                            std::uint32_t panelHeight) {
    const auto identity = connection.identity();
    if (identity == 0U) {
        return invalidContext<BottomPanelStrut>();
    }
    const bool healthy = connection.healthy();
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (!healthy || native == nullptr) {
        return transportFailure<BottomPanelStrut>();
    }
    auto context = prepareContext(native, identity, healthy, atoms.belongsTo(identity),
                                  connection.screen().rootWindow, atoms, window);
    if (!context) {
        return Result<BottomPanelStrut>::failure(context.error());
    }

    auto reservation = calculateBottomPanelStrut(root, output, panelHeight);
    if (!reservation) {
        auto removed = removeWithContext(context.value());
        if (!removed) {
            return Result<BottomPanelStrut>::failure(removed.error());
        }
        return Result<BottomPanelStrut>::failure(reservation.error());
    }

    auto published = publishReservation(context.value(), reservation.value());
    if (!published) {
        return Result<BottomPanelStrut>::failure(published.error());
    }
    return reservation;
}

Result<void> DockProperties::remove(X11Connection &connection, const AtomCache &atoms,
                                    WindowId window) {
    const auto identity = connection.identity();
    if (identity == 0U) {
        return invalidContext<void>();
    }
    const bool healthy = connection.healthy();
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (!healthy || native == nullptr) {
        return transportFailure<void>();
    }
    auto context = prepareContext(native, identity, healthy, atoms.belongsTo(identity),
                                  connection.screen().rootWindow, atoms, window);
    if (!context) {
        return Result<void>::failure(context.error());
    }
    return removeWithContext(context.value());
}

} // namespace prismdrake::x11
