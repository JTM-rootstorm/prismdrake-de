#include "TaskControllerCore.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace prismdrake::shell::taskcontroller {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

class PublishingGuard final {
  public:
    explicit PublishingGuard(bool &publishing) noexcept : publishing_(&publishing) {
        publishing = true;
    }
    ~PublishingGuard() {
        if (publishing_ != nullptr) {
            *publishing_ = false;
        }
    }

    PublishingGuard(const PublishingGuard &) = delete;
    PublishingGuard &operator=(const PublishingGuard &) = delete;

  private:
    bool *publishing_;
};

[[nodiscard]] Result<void> unavailableController(std::string message) {
    return Result<void>::failure(
        {ErrorCode::cancelled, std::move(message),
         "retain the current task state and wait for a newer complete X11 observation"});
}

} // namespace

Result<TaskEventRefreshPlan> planTaskEventRefresh(std::span<const x11::RootEvent> events) {
    if (events.size() > x11::maximumRootEventsPerDrain) {
        return Result<TaskEventRefreshPlan>::failure(
            {ErrorCode::too_large, "The task event batch exceeds its examination bound.",
             "Drain the sole X11 event stream with its documented bounded limit."});
    }

    TaskEventRefreshPlan plan;
    std::set<x11::WindowId::Value> invalidated;
    for (const auto &event : events) {
        if (const auto *topology = std::get_if<x11::ClientTopologyHint>(&event)) {
            plan.refreshRequired = true;
            if (invalidated.insert(topology->window.value()).second) {
                plan.invalidatedWindows.push_back(topology->window);
            }
            continue;
        }
        if (std::holds_alternative<x11::RootPropertyHint>(event) ||
            std::holds_alternative<x11::ClientPropertyHint>(event) ||
            std::holds_alternative<x11::ProtocolErrorHint>(event)) {
            plan.refreshRequired = true;
        }
    }
    return Result<TaskEventRefreshPlan>::success(std::move(plan));
}

bool taskEventFollowUpRequired(bool refreshAttempted, bool examinationLimitReached) noexcept {
    return refreshAttempted || examinationLimitReached;
}

TaskControllerCore::TaskControllerCore(tasks::TaskPresentationModel &presentation,
                                       CheckedDispatch dispatch, OutcomeCallback outcome,
                                       FailureCallback failure)
    : presentation_(presentation), dispatch_(std::move(dispatch)), outcome_(std::move(outcome)),
      failure_(std::move(failure)) {
    pending_.reserve(maximumPendingTaskRequests);
    activation_connection_ = QObject::connect(
        &presentation_, &tasks::TaskPresentationModel::activationRequested, &presentation_,
        [this](x11::TaskLifetimeId lifetime, x11::TaskModelGeneration generation) {
            const auto handled =
                handleIntent(lifetime, generation, x11::TaskRequestAction::activate);
            if (!handled) {
                reportFailure(handled.error());
            }
        });
    minimization_connection_ = QObject::connect(
        &presentation_, &tasks::TaskPresentationModel::minimizationRequested, &presentation_,
        [this](x11::TaskLifetimeId lifetime, x11::TaskModelGeneration generation) {
            const auto handled =
                handleIntent(lifetime, generation, x11::TaskRequestAction::minimize);
            if (!handled) {
                reportFailure(handled.error());
            }
        });
    close_connection_ = QObject::connect(
        &presentation_, &tasks::TaskPresentationModel::closeRequested, &presentation_,
        [this](x11::TaskLifetimeId lifetime, x11::TaskModelGeneration generation) {
            const auto handled = handleIntent(lifetime, generation, x11::TaskRequestAction::close);
            if (!handled) {
                reportFailure(handled.error());
            }
        });
}

TaskControllerCore::~TaskControllerCore() {
    QObject::disconnect(activation_connection_);
    QObject::disconnect(minimization_connection_);
    QObject::disconnect(close_connection_);
}

Result<std::shared_ptr<const x11::TaskModelSnapshot>>
TaskControllerCore::publishObservation(const x11::TaskModelObservation &observation) {
    if (publishing_) {
        return Result<std::shared_ptr<const x11::TaskModelSnapshot>>::failure(
            {ErrorCode::cancelled, "Task publication is already in progress.",
             "Queue the coalesced X11 refresh for a later event-loop turn."});
    }
    PublishingGuard guard{publishing_};
    auto snapshot = model_.publish(observation);
    if (!snapshot) {
        return Result<std::shared_ptr<const x11::TaskModelSnapshot>>::failure(snapshot.error());
    }

    auto applied = presentation_.applySnapshot(snapshot.value());
    if (!applied) {
        return Result<std::shared_ptr<const x11::TaskModelSnapshot>>::failure(applied.error());
    }
    evaluatePending(*snapshot.value());
    return snapshot;
}

Result<void> TaskControllerCore::handleIntent(x11::TaskLifetimeId lifetime,
                                              x11::TaskModelGeneration originatingGeneration,
                                              x11::TaskRequestAction action) {
    const auto current = model_.current();
    if (publishing_ || !current || current->generation() != originatingGeneration ||
        presentation_.currentSnapshot().get() != current.get()) {
        return unavailableController("The task request does not match the current publication.");
    }
    if (!dispatch_) {
        return Result<void>::failure(
            {ErrorCode::invalid_environment, "The checked WM request adapter is unavailable.",
             "Keep task actions disabled until the standards request path is ready."});
    }
    if (std::ranges::any_of(pending_, [lifetime, action](const auto &request) {
            return request.lifetime() == lifetime && request.action() == action;
        })) {
        return unavailableController("An equivalent task request is already pending.");
    }
    if (pending_.size() >= maximumPendingTaskRequests) {
        return Result<void>::failure(
            {ErrorCode::too_large, "The bounded pending task-request set is full.",
             "Wait for authoritative task refreshes before issuing another action."});
    }

    auto issued =
        x11::TaskRequestState::issue(*current, lifetime, action, taskRequestExpiryGenerations);
    if (!issued) {
        return Result<void>::failure(issued.error());
    }
    if (!issued.value().canDispatch(*current)) {
        return unavailableController("The task request became stale before checked delivery.");
    }

    auto delivered = dispatch_(issued.value());
    if (!delivered) {
        const auto recorded =
            issued.value().recordCheckedDelivery(x11::TaskRequestDelivery::rejected);
        if (recorded) {
            reportOutcome(issued.value(), x11::TaskRequestOutcome::deliveryRejected);
        }
        return Result<void>::failure(delivered.error());
    }
    auto recorded = issued.value().recordCheckedDelivery(x11::TaskRequestDelivery::delivered);
    if (!recorded) {
        return recorded;
    }
    pending_.push_back(std::move(issued).value());
    return Result<void>::success();
}

void TaskControllerCore::reportOutcome(const x11::TaskRequestState &request,
                                       x11::TaskRequestOutcome outcome) const {
    if (outcome_) {
        outcome_(TaskRequestUpdate{request.lifetime(), request.action(), request.issuedGeneration(),
                                   outcome});
    }
}

void TaskControllerCore::reportFailure(const Error &error) const {
    if (failure_) {
        failure_(error);
    }
}

void TaskControllerCore::evaluatePending(const x11::TaskModelSnapshot &snapshot) {
    for (auto request = pending_.begin(); request != pending_.end();) {
        const auto outcome = request->evaluate(snapshot);
        if (!x11::isTerminalTaskRequestOutcome(outcome)) {
            ++request;
            continue;
        }
        reportOutcome(*request, outcome);
        request = pending_.erase(request);
    }
}

} // namespace prismdrake::shell::taskcontroller
