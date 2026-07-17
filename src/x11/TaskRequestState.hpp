#pragma once

#include "Result.hpp"
#include "TaskModel.hpp"

#include <cstdint>

namespace prismdrake::x11 {

/// Largest caller-selected observation window for one WM-owned task request.
inline constexpr std::uint32_t maximumTaskRequestExpiryGenerations = 1024U;

enum class TaskRequestAction : std::uint8_t {
    activate,
    minimize,
    close,
};

/// Result of the checked X11 delivery operation, not the WM's eventual action.
enum class TaskRequestDelivery : std::uint8_t {
    awaitingCheck,
    delivered,
    rejected,
};

/// Display-free interpretation of delivery and subsequent authoritative state.
enum class TaskRequestOutcome : std::uint8_t {
    awaitingDeliveryCheck,
    awaitingNewerSnapshot,
    pendingConfirmation,
    deliveryRejected,
    confirmed,
    refused,
    targetDisappeared,
    targetReplaced,
};

/// Binds one WM request to one task lifetime and evaluates later snapshots.
///
/// Checked delivery only establishes that X accepted the ClientMessage. The
/// request is confirmed exclusively by a newer authoritative task snapshot.
/// This value performs no X11 I/O and never retries or redirects a request.
class TaskRequestState final {
  public:
    [[nodiscard]] static foundation::Result<TaskRequestState>
    issue(const TaskModelSnapshot &snapshot, TaskLifetimeId target, TaskRequestAction action,
          std::uint32_t expiryGenerations);

    /// Records the one checked-delivery result produced by the X11 adapter.
    /// Passing awaitingCheck or recording a second result is rejected.
    [[nodiscard]] foundation::Result<void> recordCheckedDelivery(TaskRequestDelivery delivery);

    /// Returns true only while this request is undispatched and the supplied
    /// snapshot is exactly the current generation and target incarnation used
    /// at issue time. Call this against TaskModel::current() immediately before
    /// invoking EwmhWindowRequests.
    [[nodiscard]] bool canDispatch(const TaskModelSnapshot &snapshot) const noexcept;

    [[nodiscard]] TaskRequestOutcome evaluate(const TaskModelSnapshot &snapshot) const noexcept;

    [[nodiscard]] WindowId window() const noexcept { return window_; }
    [[nodiscard]] WindowIncarnationId incarnation() const noexcept { return incarnation_; }
    [[nodiscard]] TaskLifetimeId lifetime() const noexcept { return lifetime_; }
    [[nodiscard]] TaskRequestAction action() const noexcept { return action_; }
    [[nodiscard]] TaskModelGeneration issuedGeneration() const noexcept {
        return issued_generation_;
    }
    [[nodiscard]] std::uint32_t expiryGenerations() const noexcept { return expiry_generations_; }
    [[nodiscard]] TaskRequestDelivery delivery() const noexcept { return delivery_; }

  private:
    TaskRequestState(WindowId window, WindowIncarnationId incarnation, TaskLifetimeId lifetime,
                     TaskRequestAction action, TaskModelGeneration issuedGeneration,
                     std::uint32_t expiryGenerations) noexcept;

    WindowId window_;
    WindowIncarnationId incarnation_;
    TaskLifetimeId lifetime_;
    TaskRequestAction action_;
    TaskModelGeneration issued_generation_;
    std::uint32_t expiry_generations_;
    TaskRequestDelivery delivery_{TaskRequestDelivery::awaitingCheck};
};

/// Terminal outcomes must never cause another request to the stored XID.
[[nodiscard]] bool isTerminalTaskRequestOutcome(TaskRequestOutcome outcome) noexcept;

} // namespace prismdrake::x11
