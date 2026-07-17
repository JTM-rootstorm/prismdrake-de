#include "RestartPolicy.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<void> validateBudget(const RestartBudget &budget) {
    if (budget.maximumRestarts == 0U || budget.rollingWindow <= std::chrono::milliseconds::zero() ||
        budget.healthyInterval <= std::chrono::milliseconds::zero() ||
        budget.initialBackoff < std::chrono::milliseconds::zero() ||
        budget.maximumBackoff < budget.initialBackoff) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The session restart policy is invalid.",
             "Use a positive retry budget and intervals with a bounded backoff range."});
    }
    return Result<void>::success();
}

[[nodiscard]] std::chrono::milliseconds backoffFor(const RestartBudget &budget,
                                                   std::size_t ordinal) noexcept {
    auto delay = budget.initialBackoff;
    for (std::size_t index = 1U; index < ordinal && delay < budget.maximumBackoff; ++index) {
        if (delay > budget.maximumBackoff / 2) {
            return budget.maximumBackoff;
        }
        delay *= 2;
    }
    return std::min(delay, budget.maximumBackoff);
}

} // namespace

Result<SessionRecoveryPolicy> SessionRecoveryPolicy::create(SessionRecoveryConfig config) {
    auto settingsd = validateBudget(config.settingsd);
    if (!settingsd) {
        return Result<SessionRecoveryPolicy>::failure(std::move(settingsd).error());
    }
    auto shell = validateBudget(config.shell);
    if (!shell) {
        return Result<SessionRecoveryPolicy>::failure(std::move(shell).error());
    }
    return Result<SessionRecoveryPolicy>::success(SessionRecoveryPolicy{config});
}

Result<RecoveryDecision>
SessionRecoveryPolicy::observeExit(ComponentRole component, ChildExitReason reason,
                                   foundation::MonotonicClock::TimePoint launchedAt,
                                   foundation::MonotonicClock::TimePoint observedAt) {
    if (observedAt < launchedAt) {
        return Result<RecoveryDecision>::failure(
            {ErrorCode::invalid_argument, "The child exit time precedes its launch time.",
             "Use one monotonic clock for child launch and exit observations."});
    }
    if (reason == ChildExitReason::clean) {
        return Result<RecoveryDecision>::success(
            RecoveryDecision{RecoveryAction::stop_clean, {}, 0U});
    }
    if (safe_mode_active_) {
        return Result<RecoveryDecision>::success(
            RecoveryDecision{RecoveryAction::stop_failed, {}, 0U});
    }

    const auto &componentBudget = budget(component);
    auto &componentHistory = history(component);
    if (observedAt - launchedAt >= componentBudget.healthyInterval) {
        componentHistory.exits.clear();
    }

    const auto windowStart = observedAt - componentBudget.rollingWindow;
    while (!componentHistory.exits.empty() && componentHistory.exits.front() < windowStart) {
        componentHistory.exits.pop_front();
    }

    if (componentHistory.exits.size() < componentBudget.maximumRestarts) {
        componentHistory.exits.push_back(observedAt);
        const auto ordinal = componentHistory.exits.size();
        return Result<RecoveryDecision>::success(RecoveryDecision{
            RecoveryAction::restart_component, backoffFor(componentBudget, ordinal), ordinal});
    }

    if (!safe_mode_offered_) {
        safe_mode_offered_ = true;
        return Result<RecoveryDecision>::success(
            RecoveryDecision{RecoveryAction::enter_safe_mode, {}, 0U});
    }
    return Result<RecoveryDecision>::success(RecoveryDecision{RecoveryAction::stop_failed, {}, 0U});
}

Result<void> SessionRecoveryPolicy::activateSafeMode() {
    if (!safe_mode_offered_ || safe_mode_active_) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "Safe mode cannot be activated in the current state.",
             "Activate safe mode once, immediately after an enter-safe-mode decision."});
    }
    safe_mode_active_ = true;
    settingsd_history_.exits.clear();
    shell_history_.exits.clear();
    return Result<void>::success();
}

const RestartBudget &SessionRecoveryPolicy::budget(ComponentRole component) const noexcept {
    return component == ComponentRole::settingsd ? config_.settingsd : config_.shell;
}

SessionRecoveryPolicy::ComponentHistory &
SessionRecoveryPolicy::history(ComponentRole component) noexcept {
    return component == ComponentRole::settingsd ? settingsd_history_ : shell_history_;
}

} // namespace prismdrake::session
