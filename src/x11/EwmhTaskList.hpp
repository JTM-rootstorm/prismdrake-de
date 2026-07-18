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
/// A valid stacking permutation is retained exactly as advertised. If the optional
/// stacking property is absent or temporarily names a different valid client set,
/// stackingOrder() deterministically matches clientList(). A valid stale active-window
/// hint is reduced to no active task. This value owns no windows and confers no focus
/// or stacking authority on Prismdrake.
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
    [[nodiscard]] bool stackingSetDisagreed() const noexcept { return stacking_set_disagreed_; }
    [[nodiscard]] bool staleActiveWindowCleared() const noexcept {
        return stale_active_window_cleared_;
    }
    [[nodiscard]] bool contains(WindowId window) const noexcept;

  private:
    friend foundation::Result<EwmhTaskListSnapshot>
    buildEwmhTaskListSnapshot(const EwmhTaskListObservation &observation);

    EwmhTaskListSnapshot(std::vector<WindowId> clientList, std::vector<WindowId> stackingOrder,
                         std::optional<WindowId> activeWindow, EwmhStackingSource stackingSource,
                         bool stackingSetDisagreed, bool staleActiveWindowCleared) noexcept;

    std::vector<WindowId> client_list_;
    std::vector<WindowId> stacking_order_;
    std::optional<WindowId> active_window_;
    EwmhStackingSource stacking_source_;
    bool stacking_set_disagreed_;
    bool stale_active_window_cleared_;
};

/// Validates one complete root-property observation.
///
/// The client list is required. Present lists reject zero or duplicate IDs and
/// must remain within maximumEwmhTaskWindows. A validly encoded stacking list
/// whose set temporarily differs from the mandatory client list degrades to
/// client-list order. A valid active identifier outside that list degrades to
/// no active task. Malformed encoding, zero identifiers, duplicates, and
/// oversized payloads still reject the complete observation.
[[nodiscard]] foundation::Result<EwmhTaskListSnapshot>
buildEwmhTaskListSnapshot(const EwmhTaskListObservation &observation);

/// Compares only the mandatory, ordered client-list generation. Optional
/// stacking and active-window churn cannot make stable membership incoherent.
[[nodiscard]] bool sameEwmhTaskMembership(const EwmhTaskListSnapshot &left,
                                          const EwmhTaskListSnapshot &right) noexcept;

} // namespace prismdrake::x11
