#include "EwmhWindowRequests.hpp"

#include "AtomCache.hpp"
#include "EwmhCapabilities.hpp"
#include "EwmhWindowRequestsInternal.hpp"
#include "X11Connection.hpp"

#include <cstdlib>
#include <memory>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

using ProtocolError = std::unique_ptr<xcb_generic_error_t, ReplyDeleter>;
using WindowAttributesReply = std::unique_ptr<xcb_get_window_attributes_reply_t, ReplyDeleter>;

static_assert(detail::clientMessageResponseType == XCB_CLIENT_MESSAGE);
static_assert(detail::clientMessageFormat == 32U);
static_assert(detail::substructureNotifyMask == XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY);
static_assert(detail::substructureRedirectMask == XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT);

template <typename Value> [[nodiscard]] Result<Value> transportFailure() {
    return Result<Value>::failure(
        {ErrorCode::io_error, "The X11 window-manager request transport failed.",
         "Reconnect to the X11 server and rebuild the mirrored window state."});
}

[[nodiscard]] Result<void> invalidConnection() {
    return Result<void>::failure(
        {ErrorCode::invalid_argument,
         "The window-manager request adapter belongs to another X11 connection.",
         "Create the request adapter again after reconnecting to the selected display."});
}

[[nodiscard]] Result<void> invalidTarget() {
    return Result<void>::failure({ErrorCode::invalid_argument,
                                  "The window-manager request target is not a client window.",
                                  "Use a current non-root WindowId from the mirrored task model."});
}

[[nodiscard]] Result<void> unsupportedRequest() {
    return Result<void>::failure(
        {ErrorCode::unsupported, "The active window manager does not expose this standard request.",
         "Disable the unavailable action and retain the standards-only reduced behavior."});
}

[[nodiscard]] Result<void> staleWindow() {
    return Result<void>::failure(
        {ErrorCode::not_found, "The window-manager request target is no longer a live X11 window.",
         "Remove the stale task record and rebuild it from authoritative WM state."});
}

[[nodiscard]] Result<void> probeLiveWindow(xcb_connection_t *native, WindowId window) {
    if (native == nullptr) {
        return transportFailure<void>();
    }
    xcb_generic_error_t *rawError = nullptr;
    WindowAttributesReply reply{xcb_get_window_attributes_reply(
        native, xcb_get_window_attributes(native, window.value()), &rawError)};
    ProtocolError error{rawError};
    if (xcb_connection_has_error(native) != 0) {
        return transportFailure<void>();
    }
    if (error || !reply || reply->response_type != 1U) {
        return staleWindow();
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> sendRequest(xcb_connection_t *native, WindowId root,
                                       const detail::EwmhClientMessageFields &fields) {
    auto liveTarget = probeLiveWindow(native, fields.target);
    if (!liveTarget) {
        return liveTarget;
    }
    if (native == nullptr) {
        return transportFailure<void>();
    }

    xcb_client_message_event_t event{};
    event.response_type = fields.responseType;
    event.format = fields.format;
    event.sequence = fields.sequence;
    event.window = fields.target.value();
    event.type = fields.messageType.value();
    event.data.data32[0] = fields.data[0];
    event.data.data32[1] = fields.data[1];
    event.data.data32[2] = fields.data[2];
    event.data.data32[3] = fields.data[3];
    event.data.data32[4] = fields.data[4];

    const auto cookie = xcb_send_event_checked(native, 0U, root.value(), fields.destinationMask,
                                               reinterpret_cast<const char *>(&event));
    ProtocolError error{xcb_request_check(native, cookie)};
    if (error || xcb_connection_has_error(native) != 0) {
        return transportFailure<void>();
    }
    return Result<void>::success();
}

[[nodiscard]] bool invalidTargetFor(const X11Connection &connection, WindowId target) noexcept {
    return target == connection.screen().rootWindow;
}

} // namespace

detail::EwmhClientMessageFields
detail::encodeActivateRequest(WindowId target, AtomId activeWindowAtom,
                              std::uint32_t userActionTimestamp,
                              std::optional<WindowId> currentlyActive) noexcept {
    return {clientMessageResponseType,
            clientMessageFormat,
            0U,
            target,
            activeWindowAtom,
            {pagerSourceIndication, userActionTimestamp,
             currentlyActive ? currentlyActive->value() : 0U, 0U, 0U},
            ewmhRequestEventMask};
}

detail::EwmhClientMessageFields
detail::encodeCloseRequest(WindowId target, AtomId closeWindowAtom,
                           std::uint32_t userActionTimestamp) noexcept {
    return {clientMessageResponseType,
            clientMessageFormat,
            0U,
            target,
            closeWindowAtom,
            {userActionTimestamp, pagerSourceIndication, 0U, 0U, 0U},
            ewmhRequestEventMask};
}

detail::EwmhClientMessageFields detail::encodeMinimizeRequest(WindowId target,
                                                              AtomId wmChangeStateAtom) noexcept {
    return {clientMessageResponseType,     clientMessageFormat, 0U, target, wmChangeStateAtom,
            {iconicState, 0U, 0U, 0U, 0U}, ewmhRequestEventMask};
}

Result<EwmhWindowRequests> EwmhWindowRequests::create(X11Connection &connection) {
    if (!connection.healthy() || connection.identity() == 0U) {
        return transportFailure<EwmhWindowRequests>();
    }
    auto capabilities = discoverEwmhCapabilities(connection);
    if (!capabilities) {
        return Result<EwmhWindowRequests>::failure(capabilities.error());
    }
    auto atoms = AtomCache::create(connection);
    if (!atoms) {
        return transportFailure<EwmhWindowRequests>();
    }
    const auto &flags = capabilities.value().flags;
    const bool verified = capabilities.value().status == EwmhDiscoveryStatus::verified;
    return Result<EwmhWindowRequests>::success(EwmhWindowRequests{
        connection.identity(),
        verified && flags.activeWindow ? atoms.value().atom(AtomName::net_active_window)
                                       : std::nullopt,
        verified && flags.closeWindow ? atoms.value().atom(AtomName::net_close_window)
                                      : std::nullopt,
        verified ? atoms.value().atom(AtomName::wm_change_state) : std::nullopt});
}

Result<void> EwmhWindowRequests::activate(X11Connection &connection, WindowId target,
                                          std::uint32_t userActionTimestamp,
                                          std::optional<WindowId> currentlyActive) const {
    if (connection.identity() == 0U || connection.identity() != connection_identity_) {
        return invalidConnection();
    }
    if (invalidTargetFor(connection, target) ||
        (currentlyActive && invalidTargetFor(connection, currentlyActive.value()))) {
        return invalidTarget();
    }
    if (!active_window_) {
        return unsupportedRequest();
    }
    if (!connection.healthy()) {
        return transportFailure<void>();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (currentlyActive) {
        auto liveCurrent = probeLiveWindow(native, currentlyActive.value());
        if (!liveCurrent) {
            return liveCurrent;
        }
    }
    return sendRequest(native, connection.screen().rootWindow,
                       detail::encodeActivateRequest(target, active_window_.value(),
                                                     userActionTimestamp, currentlyActive));
}

Result<void> EwmhWindowRequests::close(X11Connection &connection, WindowId target,
                                       std::uint32_t userActionTimestamp) const {
    if (connection.identity() == 0U || connection.identity() != connection_identity_) {
        return invalidConnection();
    }
    if (invalidTargetFor(connection, target)) {
        return invalidTarget();
    }
    if (!close_window_) {
        return unsupportedRequest();
    }
    if (!connection.healthy()) {
        return transportFailure<void>();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    return sendRequest(
        native, connection.screen().rootWindow,
        detail::encodeCloseRequest(target, close_window_.value(), userActionTimestamp));
}

Result<void> EwmhWindowRequests::minimize(X11Connection &connection, WindowId target) const {
    if (connection.identity() == 0U || connection.identity() != connection_identity_) {
        return invalidConnection();
    }
    if (invalidTargetFor(connection, target)) {
        return invalidTarget();
    }
    if (!wm_change_state_) {
        return unsupportedRequest();
    }
    if (!connection.healthy()) {
        return transportFailure<void>();
    }
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    return sendRequest(native, connection.screen().rootWindow,
                       detail::encodeMinimizeRequest(target, wm_change_state_.value()));
}

} // namespace prismdrake::x11
