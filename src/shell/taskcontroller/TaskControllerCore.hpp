#pragma once

#include "Result.hpp"
#include "RootEventStream.hpp"
#include "TaskModel.hpp"
#include "TaskPresentationModel.hpp"
#include "TaskRequestState.hpp"

#include <QMetaObject>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace prismdrake::shell::taskcontroller {

inline constexpr std::size_t maximumPendingTaskRequests = 64U;
inline constexpr std::uint32_t taskRequestExpiryGenerations = 8U;
inline constexpr std::array taskSnapshotStabilizationDelays{
    std::chrono::milliseconds{10}, std::chrono::milliseconds{20}, std::chrono::milliseconds{40},
    std::chrono::milliseconds{80}, std::chrono::milliseconds{160}};

/// Display-free state for one bounded deferred stabilization epoch.
class TaskSnapshotStabilizationPolicy final {
  public:
    /// Advances after one transient failure. No value means the epoch is exhausted.
    [[nodiscard]] std::optional<std::chrono::milliseconds> nextDelay() noexcept;
    void reset() noexcept {
        next_delay_ = 0U;
        exhaustion_reported_ = false;
    }
    /// A later real X event starts one new bounded epoch after exhaustion.
    void beginRealEventEpoch() noexcept {
        if (exhausted()) {
            reset();
        }
    }
    /// Returns true once per exhausted epoch for one recoverable diagnostic.
    [[nodiscard]] bool takeExhaustionReport() noexcept;
    [[nodiscard]] bool exhausted() const noexcept {
        return next_delay_ >= taskSnapshotStabilizationDelays.size();
    }
    [[nodiscard]] std::size_t scheduledAttemptCount() const noexcept { return next_delay_; }

  private:
    std::size_t next_delay_{0U};
    bool exhaustion_reported_{false};
};

/// Relevant events are coalesced while the owned single-shot timer is active.
[[nodiscard]] bool taskRefreshShouldRunImmediately(bool refreshRequired,
                                                   bool stabilizationPending) noexcept;

/// Checked WM requests remain disabled throughout deferred stabilization.
[[nodiscard]] bool taskRequestPathCanDispatch(bool adapterAvailable,
                                              bool stabilizationPending) noexcept;

struct TaskRequestUpdate final {
    x11::TaskLifetimeId lifetime;
    x11::TaskRequestAction action;
    x11::TaskModelGeneration issuedGeneration;
    x11::TaskRequestOutcome outcome;
};

struct TaskEventRefreshPlan final {
    bool refreshRequired{false};
    std::vector<x11::WindowId> invalidatedWindows;
};

/// Coalesces one bounded root-event batch into one task refresh decision.
[[nodiscard]] foundation::Result<TaskEventRefreshPlan>
planTaskEventRefresh(std::span<const x11::RootEvent> events);

/// Schedules one non-reentrant drain after a healthy refresh attempt may have buffered later XCB
/// events, or when the bounded examination limit explicitly reports remaining input.
[[nodiscard]] bool taskEventFollowUpRequired(bool refreshAttempted,
                                             bool examinationLimitReached) noexcept;

/// Display-free task publication and checked-request coordinator.
///
/// The core owns the mutable task mirror, publishes only complete immutable snapshots to the
/// passive Qt presentation, and binds every request to the exact current lifetime/generation.
class TaskControllerCore final {
  public:
    using CheckedDispatch = std::function<foundation::Result<void>(const x11::TaskRequestState &)>;
    using OutcomeCallback = std::function<void(const TaskRequestUpdate &)>;
    using FailureCallback = std::function<void(const foundation::Error &)>;

    TaskControllerCore(tasks::TaskPresentationModel &presentation, CheckedDispatch dispatch,
                       OutcomeCallback outcome = {}, FailureCallback failure = {});
    ~TaskControllerCore();

    TaskControllerCore(const TaskControllerCore &) = delete;
    TaskControllerCore &operator=(const TaskControllerCore &) = delete;

    [[nodiscard]] foundation::Result<std::shared_ptr<const x11::TaskModelSnapshot>>
    publishObservation(const x11::TaskModelObservation &observation);

    [[nodiscard]] foundation::Result<void>
    handleIntent(x11::TaskLifetimeId lifetime, x11::TaskModelGeneration originatingGeneration,
                 x11::TaskRequestAction action);

    [[nodiscard]] std::shared_ptr<const x11::TaskModelSnapshot> currentSnapshot() const noexcept {
        return model_.current();
    }
    [[nodiscard]] std::size_t pendingRequestCount() const noexcept { return pending_.size(); }
    [[nodiscard]] bool isPublishing() const noexcept { return publishing_; }

  private:
    void reportOutcome(const x11::TaskRequestState &request, x11::TaskRequestOutcome outcome) const;
    void reportFailure(const foundation::Error &error) const;
    void evaluatePending(const x11::TaskModelSnapshot &snapshot);

    tasks::TaskPresentationModel &presentation_;
    CheckedDispatch dispatch_;
    OutcomeCallback outcome_;
    FailureCallback failure_;
    x11::TaskModel model_;
    std::vector<x11::TaskRequestState> pending_;
    QMetaObject::Connection activation_connection_;
    QMetaObject::Connection minimization_connection_;
    QMetaObject::Connection close_connection_;
    bool publishing_{false};
};

} // namespace prismdrake::shell::taskcontroller
