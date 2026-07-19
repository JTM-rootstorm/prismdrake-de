#pragma once

#include "EwmhTaskSource.hpp"
#include "EwmhWindowRequests.hpp"
#include "Result.hpp"
#include "RootEventStream.hpp"
#include "TaskControllerCore.hpp"
#include "TaskIoWorker.hpp"
#include "X11Connection.hpp"

#include <QMetaObject>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class QTimer;
class QObject;

namespace prismdrake::shell::taskcontroller {

/// Event-driven standards-only X11 task controller for the development shell.
///
/// This object owns one X11 connection and its sole RootEventStream, mirrors EWMH state through
/// EwmhTaskSource, and sends only checked EWMH/ICCCM requests. It never polls or claims WM state.
/// The supplied presentation must outlive the controller.
class TaskController final {
  public:
    using ConnectionLostCallback = std::function<void(const foundation::Error &)>;
    using FailureCallback = TaskControllerCore::FailureCallback;
    using OutcomeCallback = TaskControllerCore::OutcomeCallback;

    [[nodiscard]] static foundation::Result<std::unique_ptr<TaskController>>
    create(tasks::TaskPresentationModel &presentation, std::string_view display,
           ConnectionLostCallback connectionLost, FailureCallback recoverableFailure = {},
           OutcomeCallback outcome = {});

    ~TaskController();
    TaskController(const TaskController &) = delete;
    TaskController &operator=(const TaskController &) = delete;

    [[nodiscard]] bool terminated() const noexcept { return terminated_; }
    [[nodiscard]] std::shared_ptr<const x11::TaskModelSnapshot> currentSnapshot() const noexcept {
        return core_.currentSnapshot();
    }
    [[nodiscard]] std::size_t pendingRequestCount() const noexcept {
        return core_.pendingRequestCount();
    }
    [[nodiscard]] foundation::Result<void> request(x11::TaskLifetimeId lifetime,
                                                   x11::TaskModelGeneration generation,
                                                   x11::TaskRequestAction action) {
        return core_.handleIntent(lifetime, generation, action);
    }

  private:
    TaskController(tasks::TaskPresentationModel &presentation,
                   ConnectionLostCallback connectionLost, FailureCallback recoverableFailure,
                   OutcomeCallback outcome);

    [[nodiscard]] foundation::Result<void>
    refreshTasks(std::vector<x11::WindowId> invalidatedWindows = {},
                 bool invalidateAllClients = false);
    [[nodiscard]] foundation::Result<void>
    dispatchCheckedRequest(const x11::TaskRequestState &request);
    void handleRefreshCompletion(std::uint64_t token,
                                 foundation::Result<x11::TaskModelObservation> result);
    void handleRequestCompletion(x11::TaskRequestState request, foundation::Result<void> result);
    void handleEventCompletion(foundation::Result<TaskEventRefreshPlan> result);
    void retryTaskRefresh();
    void handleRefreshFailure(const foundation::Error &error);
    void resetStabilization() noexcept;
    void reportRecoverable(const foundation::Error &error) const;
    void terminateForConnectionLoss(const foundation::Error &error);

    tasks::TaskPresentationModel &presentation_;
    ConnectionLostCallback connection_lost_;
    FailureCallback recoverable_failure_;
    TaskControllerCore core_;
    std::unique_ptr<QObject> event_context_;
    std::unique_ptr<TaskIoWorker> io_worker_;
    std::unique_ptr<QTimer> stabilization_timer_;
    TaskSnapshotStabilizationPolicy stabilization_policy_;
    std::uint64_t next_refresh_token_{1U};
    std::uint64_t requested_refresh_token_{0U};
    std::uint64_t applied_refresh_token_{0U};
    std::vector<x11::WindowId> deferred_invalidations_;
    bool deferred_full_invalidation_{false};
    bool refresh_pending_{false};
    bool terminated_{false};
};

} // namespace prismdrake::shell::taskcontroller
