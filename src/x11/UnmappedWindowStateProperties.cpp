#include "UnmappedWindowStateProperties.hpp"

#include "AtomCache.hpp"
#include "PropertyReader.hpp"
#include "X11Connection.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

inline constexpr std::size_t maximumUnmappedWindowStates = 16U;

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;
using WindowAttributes = std::unique_ptr<xcb_get_window_attributes_reply_t, FreeDeleter>;

[[nodiscard]] Result<void> invalidContext() {
    return Result<void>::failure({ErrorCode::invalid_argument,
                                  "The unmapped X11 window-state context is invalid.",
                                  "Use an atom cache created from the same live X11 connection."});
}

[[nodiscard]] Result<void> invalidTarget() {
    return Result<void>::failure({ErrorCode::invalid_argument,
                                  "The unmapped X11 window-state target is invalid.",
                                  "Use a non-root, unmapped, managed shell window."});
}

[[nodiscard]] Result<void> invalidState() {
    return Result<void>::failure(
        {ErrorCode::validation_error, "The existing unmapped X11 window state is invalid.",
         "Discard the unmapped shell surface instead of replacing malformed state."});
}

[[nodiscard]] Result<void> oversizedState() {
    return Result<void>::failure(
        {ErrorCode::too_large, "The existing unmapped X11 window state exceeds its bound.",
         "Discard the unmapped shell surface instead of extending unbounded state."});
}

[[nodiscard]] Result<void> transportFailure() {
    return Result<void>::failure(
        {ErrorCode::io_error, "The unmapped X11 window-state operation failed.",
         "Discard the unmapped shell surface and rebuild it on a live X11 connection."});
}

} // namespace

Result<void> UnmappedWindowStateProperties::publishSkipTaskbar(X11Connection &connection,
                                                               const AtomCache &atoms,
                                                               WindowId window) {
    const auto identity = connection.identity();
    if (identity == 0U || !atoms.belongsTo(identity) || window == connection.screen().rootWindow) {
        return identity == 0U || !atoms.belongsTo(identity) ? invalidContext() : invalidTarget();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    const auto stateAtom = atoms.atom(AtomName::net_wm_state);
    const auto atomType = atoms.atom(AtomName::atom);
    const auto skipTaskbar = atoms.atom(AtomName::net_wm_state_skip_taskbar);
    if (native == nullptr || !connection.healthy() || !stateAtom || !atomType || !skipTaskbar) {
        return transportFailure();
    }

    xcb_generic_error_t *rawError = nullptr;
    const auto attributesCookie = xcb_get_window_attributes(native, window.value());
    WindowAttributes attributes{
        xcb_get_window_attributes_reply(native, attributesCookie, &rawError)};
    ProtocolError attributesError{rawError};
    if (attributesError || !attributes || xcb_connection_has_error(native) != 0) {
        return transportFailure();
    }
    if (attributes->map_state != XCB_MAP_STATE_UNMAPPED || attributes->override_redirect != 0U) {
        return invalidTarget();
    }

    const PropertySpec stateSpec{AtomName::atom, PropertyFormat::bits_32,
                                 maximumUnmappedWindowStates,
                                 maximumUnmappedWindowStates * sizeof(std::uint32_t)};
    std::vector<std::uint32_t> states;
    auto existing =
        PropertyReader::read(connection, atoms, window, AtomName::net_wm_state, stateSpec);
    if (existing) {
        auto values = existing.value().uint32Items();
        if (!values) {
            return invalidState();
        }
        states = std::move(values).value();
    } else if (existing.error().code != ErrorCode::not_found) {
        return Result<void>::failure(existing.error());
    }
    if (std::set<std::uint32_t>{states.begin(), states.end()}.size() != states.size()) {
        return invalidState();
    }
    if (std::ranges::find(states, XCB_ATOM_NONE) != states.end()) {
        return invalidState();
    }
    if (std::ranges::find(states, skipTaskbar->value()) == states.end()) {
        if (states.size() >= maximumUnmappedWindowStates) {
            return oversizedState();
        }
        states.push_back(skipTaskbar->value());
    }

    const auto cookie = xcb_change_property_checked(
        native, XCB_PROP_MODE_REPLACE, window.value(), stateAtom->value(), atomType->value(), 32U,
        static_cast<std::uint32_t>(states.size()), states.data());
    ProtocolError error{xcb_request_check(native, cookie)};
    if (error || xcb_connection_has_error(native) != 0) {
        return transportFailure();
    }
    return Result<void>::success();
}

} // namespace prismdrake::x11
