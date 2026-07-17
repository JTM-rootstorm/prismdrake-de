#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <cstdint>
#include <optional>

namespace prismdrake::x11 {

class X11Connection;

/// Connection-proven EWMH and ICCCM requests to the authoritative window manager.
///
/// The adapter never changes client properties, focus, stacking, or visibility
/// directly. Each supported operation sends one checked standard ClientMessage
/// to the root window. Window identifiers remain explicit non-owning protocol IDs.
/// Callers must use a current mirrored task record. A successful result means
/// only that the checked request was delivered; authoritative state must still
/// be observed from the WM, and the target may disappear in the bounded race
/// between the liveness probe and delivery.
class EwmhWindowRequests final {
  public:
    [[nodiscard]] static foundation::Result<EwmhWindowRequests> create(X11Connection &connection);

    [[nodiscard]] bool supportsActivation() const noexcept { return active_window_.has_value(); }
    [[nodiscard]] bool supportsClose() const noexcept { return close_window_.has_value(); }
    /// ICCCM exposes no advertisement bit, so a verified EWMH WM owner is the
    /// conservative proxy before enabling WM_CHANGE_STATE minimize requests.
    [[nodiscard]] bool supportsMinimize() const noexcept { return wm_change_state_.has_value(); }

    /// Requests activation as a pager/direct-user-action source. Timestamp is
    /// the X server time associated with the user action; zero means CurrentTime.
    [[nodiscard]] foundation::Result<void>
    activate(X11Connection &connection, WindowId target, std::uint32_t userActionTimestamp,
             std::optional<WindowId> currentlyActive = std::nullopt) const;

    /// Requests WM-owned close handling; it never kills the client directly.
    [[nodiscard]] foundation::Result<void> close(X11Connection &connection, WindowId target,
                                                 std::uint32_t userActionTimestamp) const;

    /// Requests IconicState through ICCCM WM_CHANGE_STATE. It never writes
    /// `_NET_WM_STATE_HIDDEN`, which remains WM-owned observation state.
    [[nodiscard]] foundation::Result<void> minimize(X11Connection &connection,
                                                    WindowId target) const;

  private:
    EwmhWindowRequests(std::uint64_t connectionIdentity, std::optional<AtomId> activeWindow,
                       std::optional<AtomId> closeWindow,
                       std::optional<AtomId> wmChangeState) noexcept
        : connection_identity_(connectionIdentity), active_window_(activeWindow),
          close_window_(closeWindow), wm_change_state_(wmChangeState) {}

    std::uint64_t connection_identity_;
    std::optional<AtomId> active_window_;
    std::optional<AtomId> close_window_;
    std::optional<AtomId> wm_change_state_;
};

} // namespace prismdrake::x11
