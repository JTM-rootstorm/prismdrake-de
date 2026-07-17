#pragma once

#include "MonotonicClock.hpp"
#include "Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace prismdrake::session {

enum class ComponentRole : std::uint8_t {
    settingsd,
    shell,
};

enum class ChildExitReason : std::uint8_t {
    clean,
    failure,
    signal,
    readiness_timeout,
};

enum class RecoveryAction : std::uint8_t {
    stop_clean,
    restart_component,
    enter_safe_mode,
    stop_failed,
};

struct RestartBudget final {
    std::size_t maximumRestarts;
    std::chrono::milliseconds rollingWindow;
    std::chrono::milliseconds healthyInterval;
    std::chrono::milliseconds initialBackoff;
    std::chrono::milliseconds maximumBackoff;
};

struct SessionRecoveryConfig final {
    RestartBudget settingsd;
    RestartBudget shell;
};

/// Review-visible PD1 prototype policy: settingsd gets one normal restart and
/// the shell gets three, both in an inclusive rolling 30-second window.
[[nodiscard]] constexpr SessionRecoveryConfig pd1SessionRecoveryConfig() noexcept {
    using namespace std::chrono_literals;
    return {
        RestartBudget{1U, 30s, 30s, 500ms, 500ms},
        RestartBudget{3U, 30s, 30s, 250ms, 1s},
    };
}

struct RecoveryDecision final {
    RecoveryAction action;
    std::chrono::milliseconds backoff;
    std::size_t restartOrdinal;

    friend bool operator==(const RecoveryDecision &, const RecoveryDecision &) = default;
};

/// Deterministic bounded recovery state for the two required PD1 children.
///
/// Safe mode is a single final launch, not a second retry budget. Any unexpected
/// child failure while safe mode is active is terminal. Clean exits are never
/// restarted. Callers own process execution and cancellation of returned delays.
class SessionRecoveryPolicy final {
  public:
    [[nodiscard]] static foundation::Result<SessionRecoveryPolicy>
    create(SessionRecoveryConfig config = pd1SessionRecoveryConfig());

    [[nodiscard]] foundation::Result<RecoveryDecision>
    observeExit(ComponentRole component, ChildExitReason reason,
                foundation::MonotonicClock::TimePoint launchedAt,
                foundation::MonotonicClock::TimePoint observedAt);

    /// Activates the one final safe-mode launch after an enter_safe_mode decision.
    [[nodiscard]] foundation::Result<void> activateSafeMode();

    [[nodiscard]] bool safeModeActive() const noexcept { return safe_mode_active_; }

  private:
    struct ComponentHistory final {
        std::deque<foundation::MonotonicClock::TimePoint> exits;
    };

    explicit SessionRecoveryPolicy(SessionRecoveryConfig config) : config_(config) {}

    [[nodiscard]] const RestartBudget &budget(ComponentRole component) const noexcept;
    [[nodiscard]] ComponentHistory &history(ComponentRole component) noexcept;

    SessionRecoveryConfig config_;
    ComponentHistory settingsd_history_;
    ComponentHistory shell_history_;
    bool safe_mode_active_ = false;
    bool safe_mode_offered_ = false;
};

} // namespace prismdrake::session
