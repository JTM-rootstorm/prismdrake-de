#include "TaskIoWorker.hpp"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace prismdrake::shell::taskcontroller {
namespace {

using foundation::ErrorCode;
using foundation::Result;

inline constexpr std::size_t maximumQueuedTaskIoRequests = 64U;

[[nodiscard]] foundation::Error allocationFailure() {
    return {ErrorCode::too_large, "The bounded task I/O worker could not be allocated.",
            "Stop task-controller startup without publishing partial X11 state."};
}

[[nodiscard]] foundation::Error wakeFailure() {
    return {ErrorCode::io_error, "The task I/O worker wake descriptor is unavailable.",
            "Stop task-controller startup without entering an unresponsive event path."};
}

[[nodiscard]] Result<void> stoppedWorker() {
    return Result<void>::failure(
        {ErrorCode::cancelled, "The task I/O worker has stopped.",
         "Discard the operation and rebuild task state after X11 recovery."});
}

[[nodiscard]] Result<void> fullWorker() {
    return Result<void>::failure(
        {ErrorCode::too_large, "The bounded task I/O request queue is full.",
         "Wait for authoritative task refreshes before issuing another action."});
}

[[nodiscard]] Result<void> staleRequest() {
    return Result<void>::failure(
        {ErrorCode::cancelled, "The checked task request belongs to a stale refresh token.",
         "Wait for the latest authoritative task snapshot before retrying."});
}

[[nodiscard]] bool mergeInvalidations(std::vector<x11::WindowId> &target,
                                      std::span<const x11::WindowId> incoming) {
    std::set<x11::WindowId::Value> seen;
    for (const auto window : target) {
        seen.insert(window.value());
    }
    for (const auto window : incoming) {
        if (seen.insert(window.value()).second) {
            if (target.size() >= x11::maximumTaskWindows) {
                target.clear();
                return false;
            }
            target.push_back(window);
        }
    }
    return true;
}

class OperationsGuard final {
  public:
    OperationsGuard(std::atomic<TaskIoOperations *> &published, std::mutex &mutex,
                    TaskIoOperations *operations)
        : published_(published), mutex_(mutex) {
        std::scoped_lock lock{mutex_};
        published_.store(operations, std::memory_order_release);
    }
    ~OperationsGuard() {
        std::scoped_lock lock{mutex_};
        published_.store(nullptr, std::memory_order_release);
    }

  private:
    std::atomic<TaskIoOperations *> &published_;
    std::mutex &mutex_;
};

} // namespace

struct TaskIoWorker::EventDeliveryState final {
    std::mutex mutex;
    std::optional<Result<TaskEventRefreshPlan>> pending;
    EventCallback callback;
    bool queued{false};
    bool stopping{false};
};

Result<std::unique_ptr<TaskIoWorker>>
TaskIoWorker::create(QObject &owner, OperationsFactory factory, RefreshCallback refreshCallback,
                     RequestCallback requestCallback, EventCallback eventCallback) {
    if (!factory || !refreshCallback || !requestCallback || !eventCallback) {
        return Result<std::unique_ptr<TaskIoWorker>>::failure(
            {ErrorCode::invalid_argument, "The task I/O worker callbacks are incomplete.",
             "Provide one operations factory and both owner-thread completion callbacks."});
    }
    const int wakeDescriptor = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakeDescriptor < 0) {
        return Result<std::unique_ptr<TaskIoWorker>>::failure(wakeFailure());
    }
    try {
        return Result<std::unique_ptr<TaskIoWorker>>::success(std::unique_ptr<TaskIoWorker>(
            new TaskIoWorker(owner, std::move(factory), std::move(refreshCallback),
                             std::move(requestCallback), std::move(eventCallback),
                             wakeDescriptor)));
    } catch (const std::exception &) {
        (void)close(wakeDescriptor);
        return Result<std::unique_ptr<TaskIoWorker>>::failure(allocationFailure());
    }
}

TaskIoWorker::TaskIoWorker(QObject &owner, OperationsFactory factory,
                           RefreshCallback refreshCallback, RequestCallback requestCallback,
                           EventCallback eventCallback, int wakeDescriptor)
    : owner_(owner), factory_(std::move(factory)), refresh_callback_(std::move(refreshCallback)),
      request_callback_(std::move(requestCallback)), event_callback_(std::move(eventCallback)),
      wake_descriptor_(wakeDescriptor),
      event_delivery_state_(std::make_shared<EventDeliveryState>()),
      thread_([this](std::stop_token stopToken) { run(stopToken); }) {}

TaskIoWorker::~TaskIoWorker() {
    {
        std::scoped_lock lock{event_delivery_state_->mutex};
        event_delivery_state_->stopping = true;
        event_delivery_state_->pending.reset();
        event_delivery_state_->callback = {};
    }
    {
        std::scoped_lock lock{mutex_};
        stopping_ = true;
        pending_refresh_.reset();
        pending_requests_.clear();
    }
    thread_.request_stop();
    (void)wake();
    {
        std::scoped_lock lock{operations_mutex_};
        if (auto *operations = operations_.load(std::memory_order_acquire); operations != nullptr) {
            operations->cancelWaits();
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (wake_descriptor_ >= 0) {
        (void)close(wake_descriptor_);
        wake_descriptor_ = -1;
    }
}

Result<void> TaskIoWorker::submitRefresh(std::uint64_t token,
                                         std::vector<x11::WindowId> invalidatedWindows,
                                         bool invalidateAllClients) {
    if (token == 0U) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The task refresh token is invalid.",
                                      "Use one monotonically increasing non-zero worker token."});
    }
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return stoppedWorker();
        }
        if (invalidateAllClients || invalidatedWindows.size() > x11::maximumTaskWindows) {
            invalidateAllClients = true;
            invalidatedWindows.clear();
        }
        if (pending_refresh_) {
            invalidateAllClients = invalidateAllClients || pending_refresh_->invalidateAllClients;
            if (invalidateAllClients ||
                !mergeInvalidations(invalidatedWindows, pending_refresh_->invalidatedWindows)) {
                invalidateAllClients = true;
                invalidatedWindows.clear();
            }
        }
        pending_refresh_ = RefreshJob{token, std::move(invalidatedWindows), invalidateAllClients};
        if (!wake()) {
            pending_refresh_.reset();
            return Result<void>::failure(wakeFailure());
        }
    }
    return Result<void>::success();
}

Result<void> TaskIoWorker::submitRequest(std::uint64_t appliedRefreshToken,
                                         const x11::TaskRequestState &request,
                                         std::optional<x11::WindowId> currentlyActive) {
    if (appliedRefreshToken == 0U) {
        return staleRequest();
    }
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return stoppedWorker();
        }
        if (pending_requests_.size() >= maximumQueuedTaskIoRequests) {
            return fullWorker();
        }
        pending_requests_.push_back(RequestJob{appliedRefreshToken, request, currentlyActive});
        if (!wake()) {
            pending_requests_.pop_back();
            return Result<void>::failure(wakeFailure());
        }
    }
    return Result<void>::success();
}

void TaskIoWorker::run(std::stop_token stopToken) {
    auto operations = factory_();
    std::optional<foundation::Error> setupError;
    if (!operations) {
        setupError = operations.error();
    }
    OperationsGuard operationsGuard{operations_, operations_mutex_,
                                    operations ? operations.value().get() : nullptr};
    if (stopToken.stop_requested()) {
        return;
    }
    bool eventFollowUpRequired = false;
    if (operations) {
        auto initialized = operations.value()->initialize();
        if (stopToken.stop_requested()) {
            return;
        }
        if (!initialized) {
            setupError = initialized.error();
        } else {
            eventFollowUpRequired = true;
        }
    }
    std::uint64_t currentRefreshToken = 0U;

    while (!stopToken.stop_requested()) {
        bool workQueued = false;
        {
            std::scoped_lock lock{mutex_};
            workQueued =
                pending_refresh_.has_value() || !pending_requests_.empty() || eventFollowUpRequired;
        }
        const int eventDescriptor = operations ? operations.value()->eventFileDescriptor() : -1;
        std::array<pollfd, 2U> descriptors{{
            pollfd{wake_descriptor_, POLLIN, 0},
            pollfd{eventDescriptor, POLLIN, 0},
        }};
        const nfds_t descriptorCount = eventDescriptor >= 0 ? descriptors.size() : 1U;
        int pollResult = -1;
        do {
            pollResult = poll(descriptors.data(), descriptorCount, workQueued ? 0 : -1);
        } while (pollResult < 0 && errno == EINTR && !stopToken.stop_requested());
        if (stopToken.stop_requested()) {
            return;
        }
        if (pollResult < 0) {
            deliverEvent(Result<TaskEventRefreshPlan>::failure(wakeFailure()));
            return;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            std::uint64_t count = 0U;
            while (read(wake_descriptor_, &count, sizeof(count)) < 0 && errno == EINTR) {
            }
        }
        const bool eventReady = descriptorCount > 1U && (descriptors[1].revents & POLLIN) != 0;
        if (eventFollowUpRequired || eventReady) {
            eventFollowUpRequired = false;
            auto drained = operations.value()->drainEvents();
            if (drained) {
                eventFollowUpRequired = drained.value().followUpRequired;
                deliverEvent(
                    Result<TaskEventRefreshPlan>::success(std::move(drained).value().plan));
            } else {
                deliverEvent(Result<TaskEventRefreshPlan>::failure(drained.error()));
                if (drained.error().code == ErrorCode::io_error) {
                    return;
                }
            }
        } else if (descriptorCount > 1U && descriptors[1].revents != 0) {
            deliverEvent(Result<TaskEventRefreshPlan>::failure(
                {ErrorCode::io_error, "The task worker's X11 event descriptor failed.",
                 "Exit the shell and rebuild all X11 state after reconnecting."}));
            return;
        }

        std::optional<RefreshJob> refresh;
        std::optional<RequestJob> request;
        {
            std::scoped_lock lock{mutex_};
            if (stopping_ || stopToken.stop_requested()) {
                return;
            }
            if (pending_refresh_) {
                refresh = std::move(pending_refresh_);
                pending_refresh_.reset();
            } else if (!pending_requests_.empty()) {
                request = std::move(pending_requests_.front());
                pending_requests_.pop_front();
            }
        }

        if (refresh) {
            if (setupError) {
                deliverRefresh(refresh->token,
                               Result<x11::TaskModelObservation>::failure(*setupError));
                continue;
            }
            auto result = operations.value()->refresh(refresh->invalidatedWindows,
                                                      refresh->invalidateAllClients);
            eventFollowUpRequired = taskEventFollowUpRequired(true, eventFollowUpRequired);
            if (result) {
                currentRefreshToken = refresh->token;
            }
            deliverRefresh(refresh->token, std::move(result));
            continue;
        }
        if (!request) {
            continue;
        }
        if (setupError) {
            deliverRequest(std::move(request->request), Result<void>::failure(*setupError));
            continue;
        }
        if (request->appliedRefreshToken != currentRefreshToken) {
            deliverRequest(std::move(request->request), staleRequest());
            continue;
        }
        auto result = operations.value()->request(request->request, request->currentlyActive);
        eventFollowUpRequired = true;
        deliverRequest(std::move(request->request), std::move(result));
    }
}

bool TaskIoWorker::wake() noexcept {
    if (wake_descriptor_ < 0) {
        return false;
    }
    constexpr std::uint64_t increment = 1U;
    ssize_t written = -1;
    do {
        written = write(wake_descriptor_, &increment, sizeof(increment));
    } while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(increment)) || (written < 0 && errno == EAGAIN);
}

void TaskIoWorker::deliverRefresh(std::uint64_t token, Result<x11::TaskModelObservation> result) {
    auto payload = std::make_shared<Result<x11::TaskModelObservation>>(std::move(result));
    const auto callback = refresh_callback_;
    QMetaObject::invokeMethod(
        &owner_, [callback, token, payload] { callback(token, std::move(*payload)); },
        Qt::QueuedConnection);
}

void TaskIoWorker::deliverRequest(x11::TaskRequestState request, Result<void> result) {
    auto payload = std::make_shared<Result<void>>(std::move(result));
    const auto callback = request_callback_;
    QMetaObject::invokeMethod(
        &owner_,
        [callback, request = std::move(request), payload]() mutable {
            callback(std::move(request), std::move(*payload));
        },
        Qt::QueuedConnection);
}

void TaskIoWorker::deliverEvent(Result<TaskEventRefreshPlan> result) {
    auto state = event_delivery_state_;
    bool queue = false;
    {
        std::scoped_lock lock{state->mutex};
        if (state->stopping) {
            return;
        }
        if (!state->callback) {
            state->callback = event_callback_;
        }
        if (!result) {
            if (!state->pending || state->pending->hasValue() ||
                (result.error().code == ErrorCode::io_error &&
                 state->pending->error().code != ErrorCode::io_error)) {
                state->pending = std::move(result);
            }
        } else if (!state->pending) {
            auto plan = std::move(result).value();
            if (plan.invalidateAllClients ||
                plan.invalidatedWindows.size() > x11::maximumTaskWindows) {
                plan.invalidateAllClients = true;
                plan.invalidatedWindows.clear();
            }
            state->pending = Result<TaskEventRefreshPlan>::success(std::move(plan));
        } else if (state->pending->hasValue()) {
            auto &pending = state->pending->value();
            auto incoming = std::move(result).value();
            pending.refreshRequired = pending.refreshRequired || incoming.refreshRequired;
            pending.invalidateAllClients =
                pending.invalidateAllClients || incoming.invalidateAllClients;
            if (pending.invalidateAllClients ||
                !mergeInvalidations(pending.invalidatedWindows, incoming.invalidatedWindows)) {
                pending.invalidateAllClients = true;
                pending.invalidatedWindows.clear();
            }
        } else if (state->pending->error().code != ErrorCode::io_error) {
            auto plan = std::move(result).value();
            if (plan.invalidateAllClients ||
                plan.invalidatedWindows.size() > x11::maximumTaskWindows) {
                plan.invalidateAllClients = true;
                plan.invalidatedWindows.clear();
            }
            state->pending = Result<TaskEventRefreshPlan>::success(std::move(plan));
        }
        if (!state->queued) {
            state->queued = true;
            queue = true;
        }
    }
    if (queue) {
        queueEventDelivery(&owner_, state);
    }
}

void TaskIoWorker::queueEventDelivery(QObject *owner,
                                      const std::shared_ptr<EventDeliveryState> &state) {
    if (owner == nullptr) {
        return;
    }
    QPointer<QObject> guard{owner};
    const bool accepted = QMetaObject::invokeMethod(
        owner,
        [guard, state] {
            std::optional<Result<TaskEventRefreshPlan>> result;
            EventCallback callback;
            {
                std::scoped_lock lock{state->mutex};
                if (state->stopping || !state->pending) {
                    state->queued = false;
                    return;
                }
                result = std::move(state->pending);
                state->pending.reset();
                callback = state->callback;
            }
            if (callback) {
                callback(std::move(*result));
            }
            bool queueAgain = false;
            {
                std::scoped_lock lock{state->mutex};
                if (state->stopping) {
                    state->pending.reset();
                    state->queued = false;
                    return;
                }
                if (state->pending) {
                    queueAgain = true;
                } else {
                    state->queued = false;
                }
            }
            if (queueAgain && guard) {
                queueEventDelivery(guard.data(), state);
            }
        },
        Qt::QueuedConnection);
    if (!accepted) {
        std::scoped_lock lock{state->mutex};
        state->queued = false;
    }
}

} // namespace prismdrake::shell::taskcontroller
