#include "TaskController.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <exception>
#include <map>
#include <string>
#include <utility>

#include <sys/socket.h>

namespace prismdrake::shell::taskcontroller {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error missingExitHandler() {
    return {ErrorCode::invalid_argument, "The task controller connection-loss callback is missing.",
            "Provide the shell's normal X11 shutdown callback."};
}

[[nodiscard]] foundation::Error wrongThread() {
    return {ErrorCode::invalid_argument,
            "The task controller was created off its presentation thread.",
            "Create the event-driven controller on the shell's Qt owner thread."};
}

[[nodiscard]] foundation::Error lostConnection() {
    return {ErrorCode::io_error, "The task controller's X11 connection disappeared.",
            "Exit the shell normally and rebuild all X11 state after reconnecting."};
}

[[nodiscard]] foundation::Error terminatedController() {
    return {ErrorCode::cancelled, "The task controller has already stopped.",
            "Do not issue task requests after X11 connection loss."};
}

[[nodiscard]] foundation::Error allocationFailure() {
    return {ErrorCode::too_large, "The bounded task controller could not be allocated.",
            "Stop startup without publishing a partial task controller."};
}

[[nodiscard]] Result<void> staleWorkerTarget() {
    return Result<void>::failure(
        {ErrorCode::cancelled, "The checked task request target is stale on the X11 worker.",
         "Wait for the latest authoritative task snapshot before retrying."});
}

class X11TaskIoOperations final : public TaskIoOperations {
  public:
    [[nodiscard]] static Result<std::unique_ptr<TaskIoOperations>> create(std::string display) {
        auto connection = x11::X11Connection::connect(display);
        if (!connection) {
            return Result<std::unique_ptr<TaskIoOperations>>::failure(connection.error());
        }
        return Result<std::unique_ptr<TaskIoOperations>>::success(std::unique_ptr<TaskIoOperations>(
            new X11TaskIoOperations(std::move(connection).value())));
    }

    Result<void> initialize() override {
        auto events = x11::RootEventStream::create(connection_);
        if (!events) {
            return Result<void>::failure(events.error());
        }
        auto source = x11::EwmhTaskSource::create(connection_);
        if (!source) {
            return Result<void>::failure(source.error());
        }
        events_.emplace(std::move(events).value());
        source_.emplace(std::move(source).value());
        return Result<void>::success();
    }

    Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId> invalidatedWindows,
                                              bool invalidateAllClients) override {
        requests_.reset();
        if (invalidateAllClients) {
            auto source = x11::EwmhTaskSource::create(connection_);
            if (!source) {
                return Result<x11::TaskModelObservation>::failure(source.error());
            }
            source_.emplace(std::move(source).value());
        }
        for (const auto window : invalidatedWindows) {
            source_->invalidateClient(window);
        }
        auto observation = source_->refresh(connection_);
        if (!observation) {
            return Result<x11::TaskModelObservation>::failure(observation.error());
        }
        auto requests = x11::EwmhWindowRequests::create(connection_);
        if (!requests) {
            return Result<x11::TaskModelObservation>::failure(requests.error());
        }
        observed_incarnations_.clear();
        for (const auto &window : observation.value().windows) {
            observed_incarnations_.insert_or_assign(window.window.value(), window.incarnation);
        }
        requests_ = std::move(requests).value();
        return observation;
    }

    Result<void> request(const x11::TaskRequestState &request,
                         std::optional<x11::WindowId> currentlyActive) override {
        const auto found = observed_incarnations_.find(request.window().value());
        if (!requests_ || found == observed_incarnations_.end() ||
            found->second != request.incarnation()) {
            return staleWorkerTarget();
        }
        switch (request.action()) {
        case x11::TaskRequestAction::activate:
            return requests_->activate(connection_, request.window(), 0U, currentlyActive);
        case x11::TaskRequestAction::minimize:
            return requests_->minimize(connection_, request.window());
        case x11::TaskRequestAction::close:
            return requests_->close(connection_, request.window(), 0U);
        }
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The typed task request action is invalid.",
             "Issue only activate, minimize, or close through TaskPresentationModel."});
    }

    [[nodiscard]] int eventFileDescriptor() const noexcept override {
        return connection_.eventFileDescriptor();
    }

    Result<TaskEventDrain> drainEvents() override {
        if (!events_) {
            return Result<TaskEventDrain>::failure(lostConnection());
        }
        auto batch = events_->drain();
        if (!batch) {
            return Result<TaskEventDrain>::failure(batch.error());
        }
        if (!connection_.healthy()) {
            return Result<TaskEventDrain>::failure(lostConnection());
        }
        auto plan = planTaskEventRefresh(batch.value().events);
        if (!plan) {
            return Result<TaskEventDrain>::failure(plan.error());
        }
        return Result<TaskEventDrain>::success(
            {std::move(plan).value(), batch.value().examinationLimitReached});
    }

    void cancelWaits() noexcept override {
        const int descriptor = connection_.eventFileDescriptor();
        if (descriptor >= 0) {
            // Interrupt reply-bearing reads without disabling libxcb writes that may still be
            // completing on the sole worker before it observes the stop request and joins.
            (void)shutdown(descriptor, SHUT_RD);
        }
    }

  private:
    explicit X11TaskIoOperations(x11::X11Connection connection)
        : connection_(std::move(connection)) {}

    x11::X11Connection connection_;
    std::optional<x11::RootEventStream> events_;
    std::optional<x11::EwmhTaskSource> source_;
    std::optional<x11::EwmhWindowRequests> requests_;
    std::map<x11::WindowId::Value, x11::WindowIncarnationId> observed_incarnations_;
};

} // namespace

Result<std::unique_ptr<TaskController>>
TaskController::create(tasks::TaskPresentationModel &presentation, std::string_view display,
                       ConnectionLostCallback connectionLost, FailureCallback recoverableFailure,
                       OutcomeCallback outcome) {
    if (!connectionLost) {
        return Result<std::unique_ptr<TaskController>>::failure(missingExitHandler());
    }
    if (QThread::currentThread() != presentation.thread() ||
        QCoreApplication::instance() == nullptr) {
        return Result<std::unique_ptr<TaskController>>::failure(wrongThread());
    }

    std::unique_ptr<TaskController> controller;
    try {
        controller = std::unique_ptr<TaskController>(
            new TaskController(presentation, std::move(connectionLost),
                               std::move(recoverableFailure), std::move(outcome)));
        controller->event_context_ = std::make_unique<QObject>();
        controller->stabilization_timer_ =
            std::make_unique<QTimer>(controller->event_context_.get());
        controller->stabilization_timer_->setSingleShot(true);
    } catch (const std::exception &) {
        return Result<std::unique_ptr<TaskController>>::failure(allocationFailure());
    }

    QObject::connect(controller->stabilization_timer_.get(), &QTimer::timeout,
                     controller->event_context_.get(),
                     [instance = controller.get()] { instance->retryTaskRefresh(); });
    const std::string displayName{display};
    auto worker = TaskIoWorker::create(
        *controller->event_context_,
        [displayName] { return X11TaskIoOperations::create(displayName); },
        [instance = controller.get()](std::uint64_t token,
                                      Result<x11::TaskModelObservation> result) {
            instance->handleRefreshCompletion(token, std::move(result));
        },
        [instance = controller.get()](x11::TaskRequestState request, Result<void> result) {
            instance->handleRequestCompletion(std::move(request), std::move(result));
        },
        [instance = controller.get()](Result<TaskEventRefreshPlan> result) {
            instance->handleEventCompletion(std::move(result));
        });
    if (!worker) {
        return Result<std::unique_ptr<TaskController>>::failure(worker.error());
    }
    controller->io_worker_ = std::move(worker).value();
    auto initialized = controller->refreshTasks();
    if (!initialized) {
        return Result<std::unique_ptr<TaskController>>::failure(initialized.error());
    }
    return Result<std::unique_ptr<TaskController>>::success(std::move(controller));
}

TaskController::TaskController(tasks::TaskPresentationModel &presentation,
                               ConnectionLostCallback connectionLost,
                               FailureCallback recoverableFailure, OutcomeCallback outcome)
    : presentation_(presentation), connection_lost_(std::move(connectionLost)),
      recoverable_failure_(std::move(recoverableFailure)),
      core_(
          presentation,
          [this](const x11::TaskRequestState &request) { return dispatchCheckedRequest(request); },
          std::move(outcome), recoverable_failure_),
      requested_refresh_token_(0U) {}

TaskController::~TaskController() {
    if (stabilization_timer_) {
        stabilization_timer_->stop();
    }
    io_worker_.reset();
    stabilization_timer_.reset();
    event_context_.reset();
}

Result<void> TaskController::refreshTasks(std::vector<x11::WindowId> invalidatedWindows,
                                          bool invalidateAllClients) {
    if (terminated_) {
        return Result<void>::failure(terminatedController());
    }
    if (!io_worker_ || next_refresh_token_ == 0U) {
        return Result<void>::failure(allocationFailure());
    }
    const auto token = next_refresh_token_++;
    requested_refresh_token_ = token;
    refresh_pending_ = true;
    auto submitted =
        io_worker_->submitRefresh(token, std::move(invalidatedWindows), invalidateAllClients);
    if (!submitted) {
        refresh_pending_ = false;
        return submitted;
    }
    return submitted;
}

Result<void> TaskController::dispatchCheckedRequest(const x11::TaskRequestState &request) {
    if (terminated_) {
        return Result<void>::failure(terminatedController());
    }
    const auto snapshot = core_.currentSnapshot();
    const bool stabilizationPending = stabilization_timer_ && stabilization_timer_->isActive();
    if (!snapshot || !request.canDispatch(*snapshot) || refresh_pending_ ||
        !taskRequestPathCanDispatch(applied_refresh_token_ != 0U, stabilizationPending)) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "The checked task request path is not current.",
             "Wait for a complete authoritative task refresh before retrying."});
    }

    return io_worker_->submitRequest(applied_refresh_token_, request);
}

void TaskController::retryTaskRefresh() {
    if (terminated_) {
        return;
    }
    auto invalidations = std::move(deferred_invalidations_);
    deferred_invalidations_.clear();
    const bool invalidateAllClients = std::exchange(deferred_full_invalidation_, false);
    auto refreshed = refreshTasks(std::move(invalidations), invalidateAllClients);
    if (!refreshed) {
        if (refreshed.error().code == ErrorCode::io_error) {
            terminateForConnectionLoss(refreshed.error());
            return;
        }
        handleRefreshFailure(refreshed.error());
    }
}

void TaskController::handleRefreshCompletion(std::uint64_t token,
                                             Result<x11::TaskModelObservation> result) {
    if (terminated_ || token != requested_refresh_token_) {
        return;
    }
    refresh_pending_ = false;
    if (!result) {
        if (result.error().code == ErrorCode::io_error) {
            terminateForConnectionLoss(result.error());
            return;
        }
        handleRefreshFailure(result.error());
        return;
    }
    auto published = core_.publishObservation(result.value());
    if (!published) {
        handleRefreshFailure(published.error());
        return;
    }
    applied_refresh_token_ = token;
    resetStabilization();
}

void TaskController::handleRequestCompletion(x11::TaskRequestState request, Result<void> result) {
    if (terminated_ || result) {
        return;
    }
    core_.rejectPendingDelivery(request);
    if (result.error().code == ErrorCode::io_error) {
        terminateForConnectionLoss(result.error());
        return;
    }
    reportRecoverable(result.error());
}

void TaskController::handleEventCompletion(Result<TaskEventRefreshPlan> result) {
    if (terminated_) {
        return;
    }
    if (!result) {
        if (result.error().code == ErrorCode::io_error) {
            terminateForConnectionLoss(result.error());
        } else {
            reportRecoverable(result.error());
        }
        return;
    }
    auto &plan = result.value();
    mergeTaskRefreshInvalidations(plan.invalidateAllClients, plan.invalidatedWindows,
                                  deferred_full_invalidation_, deferred_invalidations_);
    const bool stabilizationPending = stabilization_timer_ && stabilization_timer_->isActive();
    if (!taskRefreshShouldRunImmediately(plan.refreshRequired, stabilizationPending)) {
        return;
    }
    stabilization_policy_.beginRealEventEpoch();
    auto invalidations = std::move(deferred_invalidations_);
    deferred_invalidations_.clear();
    const bool invalidateAllClients = std::exchange(deferred_full_invalidation_, false);
    auto refreshed = refreshTasks(std::move(invalidations), invalidateAllClients);
    if (!refreshed) {
        if (refreshed.error().code == ErrorCode::io_error) {
            terminateForConnectionLoss(refreshed.error());
        } else {
            handleRefreshFailure(refreshed.error());
        }
    }
}

void TaskController::handleRefreshFailure(const foundation::Error &error) {
    applied_refresh_token_ = 0U;
    if (!x11::taskRefreshFailureCanStabilize(error)) {
        resetStabilization();
        reportRecoverable(error);
        return;
    }
    const auto delay = stabilization_policy_.nextDelay();
    if (!delay) {
        if (stabilization_policy_.takeExhaustionReport()) {
            reportRecoverable(error);
        }
        return;
    }
    stabilization_timer_->start(static_cast<int>(delay->count()));
}

void TaskController::resetStabilization() noexcept {
    if (stabilization_timer_) {
        stabilization_timer_->stop();
    }
    stabilization_policy_.reset();
}

void TaskController::reportRecoverable(const foundation::Error &error) const {
    if (recoverable_failure_) {
        recoverable_failure_(error);
    }
}

void TaskController::terminateForConnectionLoss(const foundation::Error &error) {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    refresh_pending_ = false;
    deferred_invalidations_.clear();
    deferred_full_invalidation_ = false;
    resetStabilization();
    auto callback = connection_lost_;
    QMetaObject::invokeMethod(
        &presentation_, [callback = std::move(callback), error] { callback(error); },
        Qt::QueuedConnection);
}

} // namespace prismdrake::shell::taskcontroller
