#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace prismdrake::x11 {

/// Maximum number of root-advertised client windows accepted in one snapshot.
inline constexpr std::size_t maximumEwmhTaskWindows = 256U;

/// Identifies whether the effective stacking order came from the optional WM
/// property or the deterministic client-list fallback.
enum class EwmhStackingSource : std::uint8_t {
    clientListStacking,
    clientListFallback,
};

/// Protocol-neutral root-property observation before identifier validation.
///
/// A missing client list is distinct from a present empty list. The stacking
/// list and active window are optional EWMH properties. A present identifier
/// must always be nonzero.
struct EwmhTaskListObservation final {
    std::optional<std::vector<WindowId::Value>> clientList;
    std::optional<std::vector<WindowId::Value>> clientListStacking;
    std::optional<WindowId::Value> activeWindow;
};

/// One all-or-nothing mirror of authoritative root task-list state.
///
/// Both list orders are retained exactly as advertised. If the optional
/// stacking property is absent, stackingOrder() deterministically matches
/// clientList(). This value owns no windows and confers no focus or stacking
/// authority on Prismdrake.
class EwmhTaskListSnapshot final {
  public:
    [[nodiscard]] std::span<const WindowId> clientList() const noexcept { return client_list_; }
    [[nodiscard]] std::span<const WindowId> stackingOrder() const noexcept {
        return stacking_order_;
    }
    [[nodiscard]] const std::optional<WindowId> &activeWindow() const noexcept {
        return active_window_;
    }
    [[nodiscard]] EwmhStackingSource stackingSource() const noexcept { return stacking_source_; }
    [[nodiscard]] bool contains(WindowId window) const noexcept;

  private:
    friend foundation::Result<EwmhTaskListSnapshot>
    buildEwmhTaskListSnapshot(const EwmhTaskListObservation &observation);

    EwmhTaskListSnapshot(std::vector<WindowId> clientList, std::vector<WindowId> stackingOrder,
                         std::optional<WindowId> activeWindow,
                         EwmhStackingSource stackingSource) noexcept;

    std::vector<WindowId> client_list_;
    std::vector<WindowId> stacking_order_;
    std::optional<WindowId> active_window_;
    EwmhStackingSource stacking_source_;
};

/// Validates one complete root-property observation.
///
/// The client list is required. Present lists reject zero or duplicate IDs and
/// must remain within maximumEwmhTaskWindows. A present stacking list must be
/// an exact permutation of the client list, and a present active window must
/// be a client-list member. Any violation rejects the complete observation so
/// callers can retain their previous valid snapshot.
[[nodiscard]] foundation::Result<EwmhTaskListSnapshot>
buildEwmhTaskListSnapshot(const EwmhTaskListObservation &observation);

} // namespace prismdrake::x11
