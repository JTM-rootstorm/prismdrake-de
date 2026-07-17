#include "TaskRequestState.hpp"

#include <algorithm>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<TaskRequestState> invalidExpiry() {
    return Result<TaskRequestState>::failure(
        {ErrorCode::invalid_argument, "The task request expiry bound is invalid.",
         "Choose a nonzero bound within the documented generation limit."});
}

[[nodiscard]] Result<TaskRequestState> invalidAction() {
    return Result<TaskRequestState>::failure({ErrorCode::invalid_argument,
                                              "The task request action is invalid.",
                                              "Choose activate, minimize, or close."});
}

[[nodiscard]] Result<TaskRequestState> missingTarget() {
    return Result<TaskRequestState>::failure(
        {ErrorCode::not_found, "The task request target is not in the supplied snapshot.",
         "Select a task from the current immutable task snapshot."});
}

[[nodiscard]] bool validAction(TaskRequestAction action) noexcept {
    switch (action) {
    case TaskRequestAction::activate:
    case TaskRequestAction::minimize:
    case TaskRequestAction::close:
        return true;
    }
    return false;
}

[[nodiscard]] bool expired(TaskModelGeneration current, TaskModelGeneration issued,
                           std::uint32_t bound) noexcept {
    return current.value() > issued.value() && current.value() - issued.value() >= bound;
}

} // namespace

TaskRequestState::TaskRequestState(WindowId window, WindowIncarnationId incarnation,
                                   TaskLifetimeId lifetime, TaskRequestAction action,
                                   TaskModelGeneration issuedGeneration,
                                   std::uint32_t expiryGenerations) noexcept
    : window_(window), incarnation_(incarnation), lifetime_(lifetime), action_(action),
      issued_generation_(issuedGeneration), expiry_generations_(expiryGenerations) {}

Result<TaskRequestState> TaskRequestState::issue(const TaskModelSnapshot &snapshot,
                                                 TaskLifetimeId target, TaskRequestAction action,
                                                 std::uint32_t expiryGenerations) {
    if (expiryGenerations == 0U || expiryGenerations > maximumTaskRequestExpiryGenerations) {
        return invalidExpiry();
    }
    if (!validAction(action)) {
        return invalidAction();
    }

    const auto task =
        std::find_if(snapshot.tasks().begin(), snapshot.tasks().end(),
                     [target](const TaskRecord &record) { return record.lifetime() == target; });
    if (task == snapshot.tasks().end() || task->lastObservedGeneration() != snapshot.generation() ||
        !snapshot.authoritativelyContains(task->window(), task->incarnation())) {
        return missingTarget();
    }

    return Result<TaskRequestState>::success(
        TaskRequestState{task->window(), task->incarnation(), task->lifetime(), action,
                         snapshot.generation(), expiryGenerations});
}

Result<void> TaskRequestState::recordCheckedDelivery(TaskRequestDelivery delivery) {
    if (delivery_ != TaskRequestDelivery::awaitingCheck ||
        delivery == TaskRequestDelivery::awaitingCheck) {
        return Result<void>::failure(
            {ErrorCode::validation_error, "The checked task request delivery is invalid.",
             "Record exactly one delivered or rejected result from the X11 adapter."});
    }
    delivery_ = delivery;
    return Result<void>::success();
}

bool TaskRequestState::canDispatch(const TaskModelSnapshot &snapshot) const noexcept {
    if (delivery_ != TaskRequestDelivery::awaitingCheck ||
        snapshot.generation() != issued_generation_ ||
        !snapshot.authoritativelyContains(window_, incarnation_)) {
        return false;
    }
    return std::any_of(
        snapshot.tasks().begin(), snapshot.tasks().end(), [this](const TaskRecord &record) {
            return record.window() == window_ && record.incarnation() == incarnation_ &&
                   record.lifetime() == lifetime_ &&
                   record.lastObservedGeneration() == issued_generation_;
        });
}

TaskRequestOutcome TaskRequestState::evaluate(const TaskModelSnapshot &snapshot) const noexcept {
    if (delivery_ == TaskRequestDelivery::awaitingCheck) {
        return TaskRequestOutcome::awaitingDeliveryCheck;
    }
    if (delivery_ == TaskRequestDelivery::rejected) {
        return TaskRequestOutcome::deliveryRejected;
    }
    if (snapshot.generation().value() <= issued_generation_.value()) {
        return TaskRequestOutcome::awaitingNewerSnapshot;
    }

    const bool targetStillAuthoritative = snapshot.authoritativelyContains(window_, incarnation_);
    if (action_ == TaskRequestAction::close) {
        if (!targetStillAuthoritative) {
            return TaskRequestOutcome::confirmed;
        }
        return expired(snapshot.generation(), issued_generation_, expiry_generations_)
                   ? TaskRequestOutcome::refused
                   : TaskRequestOutcome::pendingConfirmation;
    }

    const auto task = std::find_if(
        snapshot.tasks().begin(), snapshot.tasks().end(), [this](const TaskRecord &record) {
            return record.window() == window_ && record.incarnation() == incarnation_ &&
                   record.lifetime() == lifetime_;
        });
    if (task == snapshot.tasks().end()) {
        const bool reused =
            std::any_of(snapshot.authoritativeClients().begin(),
                        snapshot.authoritativeClients().end(), [this](const auto &client) {
                            return client.window == window_ && client.incarnation != incarnation_;
                        });
        if (reused) {
            return TaskRequestOutcome::targetReplaced;
        }
        if (!targetStillAuthoritative) {
            return TaskRequestOutcome::targetDisappeared;
        }
        return expired(snapshot.generation(), issued_generation_, expiry_generations_)
                   ? TaskRequestOutcome::refused
                   : TaskRequestOutcome::pendingConfirmation;
    }

    const bool observed = action_ == TaskRequestAction::activate ? task->active() : task->hidden();
    if (observed) {
        return TaskRequestOutcome::confirmed;
    }
    return expired(snapshot.generation(), issued_generation_, expiry_generations_)
               ? TaskRequestOutcome::refused
               : TaskRequestOutcome::pendingConfirmation;
}

bool isTerminalTaskRequestOutcome(TaskRequestOutcome outcome) noexcept {
    switch (outcome) {
    case TaskRequestOutcome::awaitingDeliveryCheck:
    case TaskRequestOutcome::awaitingNewerSnapshot:
    case TaskRequestOutcome::pendingConfirmation:
        return false;
    case TaskRequestOutcome::deliveryRejected:
    case TaskRequestOutcome::confirmed:
    case TaskRequestOutcome::refused:
    case TaskRequestOutcome::targetDisappeared:
    case TaskRequestOutcome::targetReplaced:
        return true;
    }
    return true;
}

} // namespace prismdrake::x11
