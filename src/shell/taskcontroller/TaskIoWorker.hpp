#pragma once

#include "Result.hpp"
#include "TaskControllerCore.hpp"
#include "TaskModel.hpp"
#include "TaskRequestState.hpp"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace prismdrake::shell::taskcontroller {

/// Blocking X11 operations owned exclusively by TaskIoWorker's worker thread.
class TaskIoOperations {
  public:
    virtual ~TaskIoOperations() = default;

    [[nodiscard]] virtual foundation::Result<x11::TaskModelObservation>
    refresh(std::span<const x11::WindowId> invalidatedWindows) = 0;
    [[nodiscard]] virtual foundation::Result<void>
    request(const x11::TaskRequestState &request, std::optional<x11::WindowId> currentlyActive) = 0;
    [[nodiscard]] virtual int eventFileDescriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result<TaskEventRefreshPlan> drainEvents() = 0;
    virtual void cancelWaits() noexcept = 0;
};

/// One bounded worker for reply-bearing task refreshes and checked WM requests.
///
/// Refresh hints are replaceable and coalesced. Requests are bounded and are
/// rejected if their applied refresh token is no longer current when the worker
/// reaches them. Completion callbacks are always queued to the supplied owner.
class TaskIoWorker final {
  public:
    using OperationsFactory =
        std::function<foundation::Result<std::unique_ptr<TaskIoOperations>>()>;
    using RefreshCallback =
        std::function<void(std::uint64_t, foundation::Result<x11::TaskModelObservation>)>;
    using RequestCallback = std::function<void(x11::TaskRequestState, foundation::Result<void>)>;
    using EventCallback = std::function<void(foundation::Result<TaskEventRefreshPlan>)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<TaskIoWorker>>
    create(QObject &owner, OperationsFactory factory, RefreshCallback refreshCallback,
           RequestCallback requestCallback, EventCallback eventCallback);

    ~TaskIoWorker();
    TaskIoWorker(const TaskIoWorker &) = delete;
    TaskIoWorker &operator=(const TaskIoWorker &) = delete;

    [[nodiscard]] foundation::Result<void>
    submitRefresh(std::uint64_t token, std::vector<x11::WindowId> invalidatedWindows);
    [[nodiscard]] foundation::Result<void>
    submitRequest(std::uint64_t appliedRefreshToken, const x11::TaskRequestState &request,
                  std::optional<x11::WindowId> currentlyActive);

  private:
    struct RefreshJob final {
        std::uint64_t token;
        std::vector<x11::WindowId> invalidatedWindows;
    };
    struct RequestJob final {
        std::uint64_t appliedRefreshToken;
        x11::TaskRequestState request;
        std::optional<x11::WindowId> currentlyActive;
    };

    TaskIoWorker(QObject &owner, OperationsFactory factory, RefreshCallback refreshCallback,
                 RequestCallback requestCallback, EventCallback eventCallback, int wakeDescriptor);
    void run(std::stop_token stopToken);
    void deliverRefresh(std::uint64_t token, foundation::Result<x11::TaskModelObservation> result);
    void deliverRequest(x11::TaskRequestState request, foundation::Result<void> result);
    void deliverEvent(foundation::Result<TaskEventRefreshPlan> result);
    [[nodiscard]] bool wake() noexcept;

    QObject &owner_;
    OperationsFactory factory_;
    RefreshCallback refresh_callback_;
    RequestCallback request_callback_;
    EventCallback event_callback_;
    std::mutex mutex_;
    std::optional<RefreshJob> pending_refresh_;
    std::deque<RequestJob> pending_requests_;
    bool stopping_{false};
    int wake_descriptor_{-1};
    std::atomic<int> operation_descriptor_{-1};
    std::atomic<TaskIoOperations *> operations_{nullptr};
    std::jthread thread_;
};

} // namespace prismdrake::shell::taskcontroller
