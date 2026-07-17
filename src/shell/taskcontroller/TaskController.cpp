#include "TaskController.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QSocketNotifier>
#include <QThread>

#include <algorithm>
#include <exception>
#include <utility>

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

    auto connection = x11::X11Connection::connect(display);
    if (!connection) {
        return Result<std::unique_ptr<TaskController>>::failure(connection.error());
    }
    auto events = x11::RootEventStream::create(connection.value());
    if (!events) {
        return Result<std::unique_ptr<TaskController>>::failure(events.error());
    }
    auto source = x11::EwmhTaskSource::create(connection.value());
    if (!source) {
        return Result<std::unique_ptr<TaskController>>::failure(source.error());
    }

    std::unique_ptr<TaskController> controller;
    try {
        controller = std::unique_ptr<TaskController>(new TaskController(
            presentation, std::move(connection).value(), std::move(events).value(),
            std::move(source).value(), std::nullopt, std::move(connectionLost),
            std::move(recoverableFailure), std::move(outcome)));
        controller->event_context_ = std::make_unique<QObject>();
        controller->event_notifier_ = std::make_unique<QSocketNotifier>(
            controller->connection_.eventFileDescriptor(), QSocketNotifier::Read,
            controller->event_context_.get());
    } catch (const std::exception &) {
        return Result<std::unique_ptr<TaskController>>::failure(allocationFailure());
    }

    QObject::connect(controller->event_notifier_.get(), &QSocketNotifier::activated,
                     controller->event_context_.get(),
                     [instance = controller.get()](QSocketDescriptor, QSocketNotifier::Type) {
                         instance->drainEvents();
                     });
    auto initialized = controller->refreshTasks();
    if (!initialized) {
        if (!controller->connection_.healthy()) {
            return Result<std::unique_ptr<TaskController>>::failure(initialized.error());
        }
        controller->reportRecoverable(initialized.error());
    }
    return Result<std::unique_ptr<TaskController>>::success(std::move(controller));
}

TaskController::TaskController(tasks::TaskPresentationModel &presentation,
                               x11::X11Connection connection, x11::RootEventStream events,
                               x11::EwmhTaskSource source,
                               std::optional<x11::EwmhWindowRequests> requests,
                               ConnectionLostCallback connectionLost,
                               FailureCallback recoverableFailure, OutcomeCallback outcome)
    : presentation_(presentation), connection_lost_(std::move(connectionLost)),
      recoverable_failure_(std::move(recoverableFailure)),
      core_(
          presentation,
          [this](const x11::TaskRequestState &request) { return dispatchCheckedRequest(request); },
          std::move(outcome), recoverable_failure_),
      connection_(std::move(connection)), events_(std::move(events)), source_(std::move(source)),
      requests_(std::move(requests)) {}

TaskController::~TaskController() {
    if (event_notifier_) {
        event_notifier_->setEnabled(false);
    }
    event_notifier_.reset();
    event_context_.reset();
}

Result<void> TaskController::refreshTasks() {
    if (terminated_) {
        return Result<void>::failure(terminatedController());
    }
    // Any refresh hint can represent WM replacement. Disable the cached request path until a
    // matching complete observation and fresh capability check both succeed.
    requests_.reset();
    auto observation = source_.refresh(connection_);
    if (!observation) {
        return Result<void>::failure(observation.error());
    }
    auto published = core_.publishObservation(observation.value());
    if (!published) {
        return Result<void>::failure(published.error());
    }
    auto requests = x11::EwmhWindowRequests::create(connection_);
    if (!requests) {
        return Result<void>::failure(requests.error());
    }
    requests_ = std::move(requests).value();
    return Result<void>::success();
}

Result<void> TaskController::dispatchCheckedRequest(const x11::TaskRequestState &request) {
    if (terminated_ || !connection_.healthy()) {
        return Result<void>::failure(terminatedController());
    }
    const auto snapshot = core_.currentSnapshot();
    if (!snapshot || !request.canDispatch(*snapshot) || !requests_) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "The checked task request path is not current.",
             "Wait for a complete authoritative task refresh before retrying."});
    }

    switch (request.action()) {
    case x11::TaskRequestAction::activate: {
        const auto active = std::ranges::find_if(
            snapshot->tasks(), [](const x11::TaskRecord &task) { return task.active(); });
        const auto activeWindow = active == snapshot->tasks().end()
                                      ? std::optional<x11::WindowId>{}
                                      : std::optional<x11::WindowId>{active->window()};
        return requests_->activate(connection_, request.window(), 0U, activeWindow);
    }
    case x11::TaskRequestAction::minimize:
        return requests_->minimize(connection_, request.window());
    case x11::TaskRequestAction::close:
        return requests_->close(connection_, request.window(), 0U);
    }
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The typed task request action is invalid.",
         "Issue only activate, minimize, or close through TaskPresentationModel."});
}

void TaskController::drainEvents() {
    drain_scheduled_ = false;
    if (terminated_) {
        return;
    }
    auto batch = events_.drain();
    if (!batch) {
        if (!connection_.healthy()) {
            terminateForConnectionLoss(batch.error());
        } else {
            reportRecoverable(batch.error());
        }
        return;
    }
    if (!connection_.healthy()) {
        terminateForConnectionLoss(lostConnection());
        return;
    }

    auto plan = planTaskEventRefresh(batch.value().events);
    if (!plan) {
        reportRecoverable(plan.error());
        return;
    }
    for (const auto window : plan.value().invalidatedWindows) {
        source_.invalidateClient(window);
    }
    if (plan.value().refreshRequired) {
        auto refreshed = refreshTasks();
        if (!refreshed) {
            if (!connection_.healthy()) {
                terminateForConnectionLoss(refreshed.error());
                return;
            }
            reportRecoverable(refreshed.error());
        }
    }
    if (batch.value().examinationLimitReached) {
        scheduleDrain();
    }
}

void TaskController::scheduleDrain() {
    if (drain_scheduled_ || terminated_) {
        return;
    }
    drain_scheduled_ = true;
    QMetaObject::invokeMethod(
        event_context_.get(), [this] { drainEvents(); }, Qt::QueuedConnection);
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
    drain_scheduled_ = false;
    if (event_notifier_) {
        event_notifier_->setEnabled(false);
    }
    auto callback = connection_lost_;
    QMetaObject::invokeMethod(
        &presentation_, [callback = std::move(callback), error] { callback(error); },
        Qt::QueuedConnection);
}

} // namespace prismdrake::shell::taskcontroller
