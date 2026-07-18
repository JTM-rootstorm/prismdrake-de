#pragma once

#include "ChildProcess.hpp"
#include "Diagnostics.hpp"
#include "RestartPolicy.hpp"
#include "SessionEnvironment.hpp"
#include "SessionOptions.hpp"
#include "SessionRuntime.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace prismdrake::session {

inline constexpr auto sessionMonitorInterval = std::chrono::milliseconds{25};
inline constexpr auto sessionSettingsReadinessTimeout = std::chrono::seconds{5};
inline constexpr auto sessionSettingsReadinessAttemptTimeout = std::chrono::milliseconds{100};
inline constexpr auto sessionTerminateGrace = std::chrono::seconds{2};
inline constexpr auto sessionKillReapGrace = std::chrono::seconds{1};

enum class SessionTermination : std::uint8_t {
    clean,
    shutdown_requested,
};

/// Narrow process, clock, readiness, and diagnostic boundary used by the init-neutral supervisor.
///
/// Implementations own only the two Prismdrake children launched through this interface. They must
/// never signal a window manager, unrelated application, or process group.
class SessionSupervisorPlatform {
  public:
    virtual ~SessionSupervisorPlatform() = default;

    [[nodiscard]] virtual foundation::Result<void> launch(ComponentRole component,
                                                          bool safeMode) = 0;
    [[nodiscard]] virtual foundation::Result<std::optional<ChildExitStatus>>
    poll(ComponentRole component) = 0;
    [[nodiscard]] virtual bool running(ComponentRole component) const noexcept = 0;
    [[nodiscard]] virtual foundation::Result<void> requestTerminate(ComponentRole component) = 0;
    [[nodiscard]] virtual foundation::Result<void> forceKill(ComponentRole component) = 0;

    [[nodiscard]] virtual foundation::Result<void>
    waitForSettingsReadiness(std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual foundation::Result<void> markReady() = 0;
    [[nodiscard]] virtual foundation::Result<void> markSafeMode() = 0;

    [[nodiscard]] virtual foundation::MonotonicClock::TimePoint now() const noexcept = 0;
    [[nodiscard]] virtual bool shutdownRequested() const noexcept = 0;
    virtual void waitFor(std::chrono::milliseconds duration) = 0;
    virtual void publishDiagnostic(const foundation::DiagnosticEvent &event) = 0;
};

/// Single-threaded development-session controller with bounded recovery and reverse shutdown.
class SessionSupervisor final {
  public:
    [[nodiscard]] static foundation::Result<SessionSupervisor>
    create(SessionSupervisorPlatform &platform,
           SessionRecoveryConfig recoveryConfig = pd1SessionRecoveryConfig());

    [[nodiscard]] foundation::Result<SessionTermination> run();

  private:
    SessionSupervisor(SessionSupervisorPlatform &platform, SessionRecoveryPolicy recoveryPolicy)
        : platform_(&platform), recovery_policy_(std::move(recoveryPolicy)) {}

    enum class Progress : std::uint8_t {
        running,
        clean_stop,
        enter_safe_mode,
        shutdown_requested,
    };

    [[nodiscard]] foundation::Result<Progress> startUntilStable(ComponentRole component);
    [[nodiscard]] foundation::Result<Progress>
    recover(ComponentRole component, ChildExitReason reason,
            foundation::MonotonicClock::TimePoint launchedAt);
    [[nodiscard]] foundation::Result<Progress> handleExit(ComponentRole component,
                                                          const ChildExitStatus &status);
    [[nodiscard]] foundation::Result<Progress> enterSafeMode();
    [[nodiscard]] foundation::Result<void> stopComponent(ComponentRole component);
    [[nodiscard]] foundation::Result<void> shutdownAll();
    [[nodiscard]] bool waitInterruptibly(std::chrono::milliseconds duration);
    void publishComponentFailure(ComponentRole component, foundation::DiagnosticRecovery recovery);
    void publishForcedTermination(ComponentRole component);

    [[nodiscard]] std::optional<foundation::MonotonicClock::TimePoint> &
    launchTime(ComponentRole component) noexcept;

    SessionSupervisorPlatform *platform_;
    SessionRecoveryPolicy recovery_policy_;
    std::optional<foundation::MonotonicClock::TimePoint> settingsd_launched_at_;
    std::optional<foundation::MonotonicClock::TimePoint> shell_launched_at_;
};

/// Creates the real exact-PID backend used by prismdrake-session.
[[nodiscard]] std::unique_ptr<SessionSupervisorPlatform> makeProductionSessionSupervisorPlatform(
    SessionRuntimeOptions options, PreparedSessionEnvironment environment, SessionRuntime &runtime);

/// Installs SIGINT and SIGTERM handlers that only set the supervisor's shutdown flag.
[[nodiscard]] foundation::Result<void> installSessionSignalHandlers();

} // namespace prismdrake::session
