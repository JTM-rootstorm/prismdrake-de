#include "SessionSupervisor.hpp"

#include "SettingsReadiness.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <iostream>
#include <thread>
#include <utility>

namespace prismdrake::session {
namespace {

using foundation::DiagnosticComponent;
using foundation::DiagnosticEventId;
using foundation::DiagnosticRecovery;
using foundation::DiagnosticSeverity;
using foundation::Error;
using foundation::ErrorCode;
using foundation::MonotonicClock;
using foundation::Result;

volatile std::sig_atomic_t sessionShutdownRequested = 0;

extern "C" void requestSessionShutdown(int) { sessionShutdownRequested = 1; }

[[nodiscard]] constexpr DiagnosticComponent diagnosticComponent(ComponentRole component) noexcept {
    return component == ComponentRole::settingsd ? DiagnosticComponent::settingsd
                                                 : DiagnosticComponent::shell;
}

[[nodiscard]] constexpr ChildExitReason exitReason(const ChildExitStatus &status) noexcept {
    if (status.kind == ChildExitKind::signaled) {
        return ChildExitReason::signal;
    }
    return status.value == 0 ? ChildExitReason::clean : ChildExitReason::failure;
}

[[nodiscard]] Error terminalComponentFailure() {
    return {ErrorCode::io_error, "A supervised component exhausted bounded recovery.",
            "Inspect structured session diagnostics and restart the development session."};
}

[[nodiscard]] Error supervisionFailure() {
    return {ErrorCode::io_error, "The development session could not supervise an owned child.",
            "Stop the development session and inspect structured component diagnostics."};
}

[[nodiscard]] Error settingsReadinessContractFailure() {
    return {ErrorCode::validation_error,
            "The settings service published an invalid readiness contract.",
            "Stop the development session and inspect fixed settings-service diagnostics."};
}

[[nodiscard]] constexpr bool permanentReadinessError(ErrorCode code) noexcept {
    return code == ErrorCode::invalid_argument || code == ErrorCode::validation_error ||
           code == ErrorCode::unsupported;
}

[[nodiscard]] std::chrono::milliseconds
waitSliceUntil(MonotonicClock::TimePoint deadline, MonotonicClock::TimePoint now,
               std::chrono::milliseconds maximumSlice = sessionMonitorInterval) noexcept {
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    const auto remaining = deadline - now;
    auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (rounded < remaining) {
        rounded += std::chrono::milliseconds{1};
    }
    return std::min(maximumSlice, rounded);
}

class ProductionSessionSupervisorPlatform final : public SessionSupervisorPlatform {
  public:
    ProductionSessionSupervisorPlatform(SessionRuntimeOptions options,
                                        PreparedSessionEnvironment environment,
                                        SessionRuntime &runtime)
        : options_(std::move(options)), environment_(std::move(environment)), runtime_(&runtime) {}

    Result<void> launch(ComponentRole component, bool safeMode) override {
        auto &child = process(component);
        if (child && child->waitable()) {
            return Result<void>::failure(
                {ErrorCode::invalid_argument, "A supervised component is already running.",
                 "Collect or stop the current exact child before launching a replacement."});
        }

        ChildLaunch launch;
        if (component == ComponentRole::settingsd) {
            launch.executable = options_.settingsdExecutable;
            launch.argv = {"prismdrake-settingsd", "--foreground"};
            if (safeMode) {
                launch.argv.emplace_back("--safe-mode");
            }
        } else {
            launch.executable = options_.shellExecutable;
            launch.argv = {"prismdrake-shell"};
        }

        auto launched = launchChildProcess(launch, environment_);
        if (!launched) {
            return Result<void>::failure(launched.error());
        }
        child.reset();
        child.emplace(std::move(launched).value());
        return Result<void>::success();
    }

    Result<std::optional<ChildExitStatus>> poll(ComponentRole component) override {
        auto &child = process(component);
        if (!child || !child->waitable()) {
            return Result<std::optional<ChildExitStatus>>::failure(
                {ErrorCode::invalid_argument, "A supervised component is not pollable.",
                 "Poll only an exact child currently owned by the session."});
        }
        auto status = child->waitNonBlocking();
        if (!status) {
            // Preserve ownership after a transient wait failure so the controller can still
            // terminate and reap the exact PID during its bounded shutdown path. ECHILD is the
            // only wait failure that invalidates ChildProcess ownership itself.
            if (!child->waitable()) {
                child.reset();
            }
            return Result<std::optional<ChildExitStatus>>::failure(status.error());
        }
        if (status.value()) {
            child.reset();
        }
        return status;
    }

    bool running(ComponentRole component) const noexcept override {
        const auto &child = process(component);
        return child && child->waitable();
    }

    Result<void> requestTerminate(ComponentRole component) override {
        auto &child = process(component);
        if (!child || !child->waitable()) {
            return Result<void>::success();
        }
        return child->sendTerminate();
    }

    Result<void> forceKill(ComponentRole component) override {
        auto &child = process(component);
        if (!child || !child->waitable()) {
            return Result<void>::success();
        }
        return child->sendKill();
    }

    Result<void> waitForSettingsReadiness(std::chrono::milliseconds timeout) override {
        auto ready = probeSettingsReadiness(timeout);
        if (!ready) {
            return Result<void>::failure(ready.error());
        }
        return Result<void>::success();
    }

    Result<void> markReady() override { return runtime_->markReady(); }
    Result<void> markSafeMode() override { return runtime_->markSafeMode(); }

    MonotonicClock::TimePoint now() const noexcept override { return MonotonicClock::Clock::now(); }
    bool shutdownRequested() const noexcept override { return sessionShutdownRequested != 0; }

    void waitFor(std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
    }

    void publishDiagnostic(const foundation::DiagnosticEvent &event) override {
        std::cerr << foundation::renderDiagnosticEvent(event) << '\n';
    }

  private:
    [[nodiscard]] std::optional<ChildProcess> &process(ComponentRole component) noexcept {
        return component == ComponentRole::settingsd ? settingsd_ : shell_;
    }
    [[nodiscard]] const std::optional<ChildProcess> &
    process(ComponentRole component) const noexcept {
        return component == ComponentRole::settingsd ? settingsd_ : shell_;
    }

    SessionRuntimeOptions options_;
    PreparedSessionEnvironment environment_;
    SessionRuntime *runtime_;
    std::optional<ChildProcess> settingsd_;
    std::optional<ChildProcess> shell_;
};

} // namespace

Result<SessionSupervisor> SessionSupervisor::create(SessionSupervisorPlatform &platform,
                                                    SessionRecoveryConfig recoveryConfig) {
    auto policy = SessionRecoveryPolicy::create(recoveryConfig);
    if (!policy) {
        return Result<SessionSupervisor>::failure(policy.error());
    }
    return Result<SessionSupervisor>::success(
        SessionSupervisor{platform, std::move(policy).value()});
}

Result<SessionTermination> SessionSupervisor::run() {
    if (platform_->shutdownRequested()) {
        return Result<SessionTermination>::success(SessionTermination::shutdown_requested);
    }

    auto settings = startUntilStable(ComponentRole::settingsd);
    if (!settings) {
        static_cast<void>(shutdownAll());
        return Result<SessionTermination>::failure(settings.error());
    }
    if (settings.value() == Progress::enter_safe_mode) {
        auto safeMode = enterSafeMode();
        if (!safeMode) {
            static_cast<void>(shutdownAll());
            return Result<SessionTermination>::failure(safeMode.error());
        }
        if (safeMode.value() == Progress::clean_stop) {
            auto stopped = shutdownAll();
            return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                           : Result<SessionTermination>::failure(stopped.error());
        }
        if (safeMode.value() == Progress::shutdown_requested) {
            auto stopped = shutdownAll();
            return stopped
                       ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
        }
    } else if (settings.value() == Progress::clean_stop) {
        auto stopped = shutdownAll();
        return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                       : Result<SessionTermination>::failure(stopped.error());
    } else if (settings.value() == Progress::shutdown_requested) {
        auto stopped = shutdownAll();
        return stopped ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
    }

    if (!recovery_policy_.safeModeActive()) {
        auto shell = startUntilStable(ComponentRole::shell);
        if (!shell) {
            static_cast<void>(shutdownAll());
            return Result<SessionTermination>::failure(shell.error());
        }
        if (shell.value() == Progress::enter_safe_mode) {
            auto safeMode = enterSafeMode();
            if (!safeMode) {
                static_cast<void>(shutdownAll());
                return Result<SessionTermination>::failure(safeMode.error());
            }
            if (safeMode.value() == Progress::clean_stop) {
                auto stopped = shutdownAll();
                return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                               : Result<SessionTermination>::failure(stopped.error());
            }
            if (safeMode.value() == Progress::shutdown_requested) {
                auto stopped = shutdownAll();
                return stopped ? Result<SessionTermination>::success(
                                     SessionTermination::shutdown_requested)
                               : Result<SessionTermination>::failure(stopped.error());
            }
        } else if (shell.value() == Progress::shutdown_requested) {
            auto stopped = shutdownAll();
            return stopped
                       ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
        }
    }

    auto ready = platform_->markReady();
    if (!ready) {
        static_cast<void>(shutdownAll());
        return Result<SessionTermination>::failure(ready.error());
    }

    for (;;) {
        if (platform_->shutdownRequested()) {
            platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
                DiagnosticComponent::session, DiagnosticSeverity::info,
                DiagnosticEventId::operation_cancelled, std::nullopt, std::nullopt,
                DiagnosticRecovery::none));
            auto stopped = shutdownAll();
            return stopped
                       ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
        }

        for (const auto component : {ComponentRole::shell, ComponentRole::settingsd}) {
            if (!platform_->running(component)) {
                continue;
            }
            auto status = platform_->poll(component);
            if (!status) {
                static_cast<void>(shutdownAll());
                return Result<SessionTermination>::failure(supervisionFailure());
            }
            if (!status.value()) {
                continue;
            }

            auto progress = handleExit(component, *status.value());
            if (!progress) {
                static_cast<void>(shutdownAll());
                return Result<SessionTermination>::failure(progress.error());
            }
            if (progress.value() == Progress::clean_stop) {
                auto stopped = shutdownAll();
                return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                               : Result<SessionTermination>::failure(stopped.error());
            }
            if (progress.value() == Progress::shutdown_requested) {
                auto stopped = shutdownAll();
                return stopped ? Result<SessionTermination>::success(
                                     SessionTermination::shutdown_requested)
                               : Result<SessionTermination>::failure(stopped.error());
            }
            if (progress.value() == Progress::enter_safe_mode) {
                auto safeMode = enterSafeMode();
                if (!safeMode) {
                    static_cast<void>(shutdownAll());
                    return Result<SessionTermination>::failure(safeMode.error());
                }
                if (safeMode.value() == Progress::clean_stop) {
                    auto stopped = shutdownAll();
                    return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                                   : Result<SessionTermination>::failure(stopped.error());
                }
                if (safeMode.value() == Progress::shutdown_requested) {
                    auto stopped = shutdownAll();
                    return stopped ? Result<SessionTermination>::success(
                                         SessionTermination::shutdown_requested)
                                   : Result<SessionTermination>::failure(stopped.error());
                }
                break;
            }
        }
        platform_->waitFor(sessionMonitorInterval);
    }
}

Result<SessionSupervisor::Progress> SessionSupervisor::startUntilStable(ComponentRole component) {
    for (;;) {
        if (platform_->shutdownRequested()) {
            return Result<Progress>::success(Progress::shutdown_requested);
        }
        auto &launchedAt = launchTime(component);
        launchedAt = platform_->now();
        auto launched = platform_->launch(component, recovery_policy_.safeModeActive());
        if (!launched) {
            launchedAt.reset();
            auto recovery = recover(component, ChildExitReason::failure, platform_->now());
            if (!recovery || recovery.value() != Progress::running) {
                return recovery;
            }
            continue;
        }

        if (component != ComponentRole::settingsd) {
            return Result<Progress>::success(Progress::running);
        }

        const auto readinessDeadline = platform_->now() + sessionSettingsReadinessTimeout;
        for (;;) {
            if (platform_->shutdownRequested()) {
                auto stopped = stopComponent(component);
                return stopped ? Result<Progress>::success(Progress::shutdown_requested)
                               : Result<Progress>::failure(stopped.error());
            }

            auto status = platform_->poll(component);
            if (!status) {
                static_cast<void>(stopComponent(component));
                return Result<Progress>::failure(supervisionFailure());
            }
            if (status.value()) {
                const auto launchObservation = launchedAt.value_or(platform_->now());
                launchedAt.reset();
                auto recovery = recover(component, exitReason(*status.value()), launchObservation);
                if (!recovery || recovery.value() != Progress::running) {
                    return recovery;
                }
                break;
            }

            const auto attemptTimeout = waitSliceUntil(readinessDeadline, platform_->now(),
                                                       sessionSettingsReadinessAttemptTimeout);
            if (attemptTimeout == std::chrono::milliseconds::zero()) {
                auto stopped = stopComponent(component);
                if (!stopped) {
                    return Result<Progress>::failure(stopped.error());
                }
                const auto launchObservation = launchedAt.value_or(platform_->now());
                launchedAt.reset();
                auto recovery =
                    recover(component, ChildExitReason::readiness_timeout, launchObservation);
                if (!recovery || recovery.value() != Progress::running) {
                    return recovery;
                }
                break;
            }

            auto ready = platform_->waitForSettingsReadiness(attemptTimeout);
            if (ready) {
                return Result<Progress>::success(Progress::running);
            }
            if (ready.error().code != ErrorCode::not_found) {
                auto stopped = stopComponent(component);
                if (!stopped) {
                    return Result<Progress>::failure(stopped.error());
                }
                if (permanentReadinessError(ready.error().code)) {
                    return Result<Progress>::failure(settingsReadinessContractFailure());
                }
                const auto launchObservation = launchedAt.value_or(platform_->now());
                launchedAt.reset();
                auto recovery = recover(component, ChildExitReason::failure, launchObservation);
                if (!recovery || recovery.value() != Progress::running) {
                    return recovery;
                }
                break;
            }

            const auto retryDelay = waitSliceUntil(readinessDeadline, platform_->now());
            if (retryDelay > std::chrono::milliseconds::zero() && !waitInterruptibly(retryDelay)) {
                auto stopped = stopComponent(component);
                return stopped ? Result<Progress>::success(Progress::shutdown_requested)
                               : Result<Progress>::failure(stopped.error());
            }
        }
    }
}

Result<SessionSupervisor::Progress>
SessionSupervisor::recover(ComponentRole component, ChildExitReason reason,
                           MonotonicClock::TimePoint launchedAt) {
    auto decision = recovery_policy_.observeExit(component, reason, launchedAt, platform_->now());
    if (!decision) {
        return Result<Progress>::failure(decision.error());
    }

    switch (decision.value().action) {
    case RecoveryAction::stop_clean:
        platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
            diagnosticComponent(component), DiagnosticSeverity::info,
            DiagnosticEventId::operation_cancelled));
        return Result<Progress>::success(Progress::clean_stop);
    case RecoveryAction::restart_component:
        publishComponentFailure(component, DiagnosticRecovery::restart_component);
        return Result<Progress>::success(waitInterruptibly(decision.value().backoff)
                                             ? Progress::running
                                             : Progress::shutdown_requested);
    case RecoveryAction::enter_safe_mode:
        publishComponentFailure(component, DiagnosticRecovery::use_fallback);
        return Result<Progress>::success(Progress::enter_safe_mode);
    case RecoveryAction::stop_failed:
        platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
            diagnosticComponent(component), DiagnosticSeverity::critical,
            DiagnosticEventId::component_restart_exhausted, std::nullopt, std::nullopt,
            DiagnosticRecovery::none));
        return Result<Progress>::failure(terminalComponentFailure());
    }
    return Result<Progress>::failure(supervisionFailure());
}

Result<SessionSupervisor::Progress> SessionSupervisor::handleExit(ComponentRole component,
                                                                  const ChildExitStatus &status) {
    auto &launchedAt = launchTime(component);
    if (!launchedAt) {
        return Result<Progress>::failure(supervisionFailure());
    }
    const auto observation = *launchedAt;
    launchedAt.reset();
    auto recovery = recover(component, exitReason(status), observation);
    if (!recovery || recovery.value() != Progress::running) {
        return recovery;
    }
    return startUntilStable(component);
}

Result<SessionSupervisor::Progress> SessionSupervisor::enterSafeMode() {
    auto stopped = shutdownAll();
    if (!stopped) {
        return Result<Progress>::failure(stopped.error());
    }
    auto activated = recovery_policy_.activateSafeMode();
    if (!activated) {
        return Result<Progress>::failure(activated.error());
    }
    auto marker = platform_->markSafeMode();
    if (!marker) {
        return Result<Progress>::failure(marker.error());
    }
    platform_->publishDiagnostic(
        foundation::makeDiagnosticEvent(DiagnosticComponent::session, DiagnosticSeverity::warning,
                                        DiagnosticEventId::fallback_selected, std::nullopt,
                                        std::nullopt, DiagnosticRecovery::use_fallback));

    auto settings = startUntilStable(ComponentRole::settingsd);
    if (!settings) {
        return Result<Progress>::failure(settings.error());
    }
    if (settings.value() == Progress::shutdown_requested) {
        return settings;
    }
    if (settings.value() == Progress::clean_stop) {
        return settings;
    }
    if (settings.value() != Progress::running) {
        return Result<Progress>::failure(terminalComponentFailure());
    }
    auto shell = startUntilStable(ComponentRole::shell);
    if (!shell) {
        return Result<Progress>::failure(shell.error());
    }
    if (shell.value() == Progress::shutdown_requested) {
        return shell;
    }
    if (shell.value() != Progress::running) {
        return Result<Progress>::failure(terminalComponentFailure());
    }
    return Result<Progress>::success(Progress::running);
}

Result<void> SessionSupervisor::stopComponent(ComponentRole component) {
    if (!platform_->running(component)) {
        launchTime(component).reset();
        return Result<void>::success();
    }

    std::optional<Error> firstError;
    auto terminated = platform_->requestTerminate(component);
    if (!terminated) {
        firstError = terminated.error();
    }
    const auto terminateDeadline = platform_->now() + sessionTerminateGrace;
    while (platform_->running(component) && platform_->now() < terminateDeadline) {
        auto status = platform_->poll(component);
        if (!status) {
            if (!firstError) {
                firstError = status.error();
            }
            break;
        }
        if (status.value()) {
            launchTime(component).reset();
            return firstError ? Result<void>::failure(*firstError) : Result<void>::success();
        }
        platform_->waitFor(waitSliceUntil(terminateDeadline, platform_->now()));
    }

    if (platform_->running(component)) {
        publishForcedTermination(component);
        auto killed = platform_->forceKill(component);
        if (!killed && !firstError) {
            firstError = killed.error();
        }
        const auto killDeadline = platform_->now() + sessionKillReapGrace;
        while (platform_->running(component) && platform_->now() < killDeadline) {
            auto status = platform_->poll(component);
            if (!status) {
                if (!firstError) {
                    firstError = status.error();
                }
                break;
            }
            if (status.value()) {
                launchTime(component).reset();
                return firstError ? Result<void>::failure(*firstError) : Result<void>::success();
            }
            platform_->waitFor(waitSliceUntil(killDeadline, platform_->now()));
        }
    }

    launchTime(component).reset();
    if (firstError) {
        return Result<void>::failure(*firstError);
    }
    if (platform_->running(component)) {
        return Result<void>::failure({ErrorCode::io_error,
                                      "A supervised child exceeded bounded forced shutdown.",
                                      "Inspect the exact child process and session diagnostics."});
    }
    return Result<void>::success();
}

Result<void> SessionSupervisor::shutdownAll() {
    std::optional<Error> firstError;
    for (const auto component : {ComponentRole::shell, ComponentRole::settingsd}) {
        auto stopped = stopComponent(component);
        if (!stopped && !firstError) {
            firstError = stopped.error();
        }
    }
    if (firstError) {
        return Result<void>::failure(*firstError);
    }
    return Result<void>::success();
}

bool SessionSupervisor::waitInterruptibly(std::chrono::milliseconds duration) {
    auto remaining = duration;
    while (remaining > std::chrono::milliseconds::zero()) {
        if (platform_->shutdownRequested()) {
            return false;
        }
        const auto slice = std::min(sessionMonitorInterval, remaining);
        platform_->waitFor(slice);
        remaining -= slice;
    }
    return !platform_->shutdownRequested();
}

void SessionSupervisor::publishComponentFailure(ComponentRole component,
                                                DiagnosticRecovery recovery) {
    platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
        diagnosticComponent(component), DiagnosticSeverity::error,
        DiagnosticEventId::component_start_failed, std::nullopt, std::nullopt, recovery));
}

void SessionSupervisor::publishForcedTermination(ComponentRole component) {
    platform_->publishDiagnostic(
        foundation::makeDiagnosticEvent(diagnosticComponent(component), DiagnosticSeverity::warning,
                                        DiagnosticEventId::operation_cancelled));
}

std::optional<MonotonicClock::TimePoint> &
SessionSupervisor::launchTime(ComponentRole component) noexcept {
    return component == ComponentRole::settingsd ? settingsd_launched_at_ : shell_launched_at_;
}

std::unique_ptr<SessionSupervisorPlatform>
makeProductionSessionSupervisorPlatform(SessionRuntimeOptions options,
                                        PreparedSessionEnvironment environment,
                                        SessionRuntime &runtime) {
    return std::make_unique<ProductionSessionSupervisorPlatform>(std::move(options),
                                                                 std::move(environment), runtime);
}

Result<void> installSessionSignalHandlers() {
    sessionShutdownRequested = 0;
    struct sigaction action = {};
    action.sa_handler = requestSessionShutdown;
    if (::sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGINT, &action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &action, nullptr) != 0) {
        return Result<void>::failure(
            {ErrorCode::io_error, "Session shutdown handlers could not be installed.",
             "Retry the development session in a valid POSIX process environment."});
    }
    return Result<void>::success();
}

} // namespace prismdrake::session
