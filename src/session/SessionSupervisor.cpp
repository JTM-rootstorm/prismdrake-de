#include "SessionSupervisor.hpp"

#include "NotificationReadiness.hpp"
#include "SessionControlService.hpp"
#include "SessionReadinessProtocol.hpp"
#include "SettingsReadiness.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

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
volatile std::sig_atomic_t sessionActivityWriteDescriptor = -1;
int sessionActivityReadDescriptor = -1;

extern "C" void wakeSessionSupervisor(int signal) {
    const int savedError = errno;
    if (signal == SIGINT || signal == SIGTERM) {
        sessionShutdownRequested = 1;
    }
    const int descriptor = sessionActivityWriteDescriptor;
    if (descriptor >= 0) {
        constexpr std::uint8_t wake = 1U;
        static_cast<void>(::write(descriptor, &wake, sizeof(wake)));
    }
    errno = savedError;
}

[[nodiscard]] constexpr DiagnosticComponent diagnosticComponent(ComponentRole component) noexcept {
    switch (component) {
    case ComponentRole::settingsd:
        return DiagnosticComponent::settingsd;
    case ComponentRole::notifyd:
        return DiagnosticComponent::notifyd;
    case ComponentRole::shell:
        return DiagnosticComponent::shell;
    }
    return DiagnosticComponent::session;
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

[[nodiscard]] Error shellReadinessContractFailure() {
    return {ErrorCode::validation_error, "The shell published an invalid readiness contract.",
            "Stop the development session and verify matching Prismdrake components."};
}

[[nodiscard]] Error sessionControlFailure(ErrorCode code = ErrorCode::io_error) {
    return {code, "The Experimental session-control service could not be owned.",
            "Stop the conflicting owner or reconnect to the user session bus."};
}

[[nodiscard]] Error activityWaitFailure() {
    return {ErrorCode::io_error, "The session activity wait could not be maintained.",
            "Stop the development session and inspect its session bus and signal channel."};
}

void closeDescriptor(int &descriptor) noexcept {
    if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
        descriptor = -1;
    }
}

[[nodiscard]] bool environmentEntryNamed(std::string_view entry, std::string_view name) noexcept {
    return entry.size() > name.size() && entry[name.size()] == '=' && entry.starts_with(name);
}

[[nodiscard]] Result<int> createShellReadinessDescriptor() {
    int descriptor = -1;
    do {
        descriptor = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return Result<int>::failure(
            {ErrorCode::io_error, "The shell readiness channel could not be created.",
             "Retry the session after checking process descriptor limits."});
    }
    return Result<int>::success(descriptor);
}

[[nodiscard]] constexpr bool permanentReadinessError(ErrorCode code) noexcept {
    return code == ErrorCode::invalid_argument || code == ErrorCode::validation_error ||
           code == ErrorCode::unsupported;
}

[[nodiscard]] std::chrono::milliseconds
waitSliceUntil(MonotonicClock::TimePoint deadline, MonotonicClock::TimePoint now,
               std::chrono::milliseconds maximumSlice = sessionInterruptibleWaitSlice) noexcept {
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

    ~ProductionSessionSupervisorPlatform() override {
        closeDescriptor(shell_readiness_descriptor_);
    }

    Result<void> launch(ComponentRole component, bool safeMode) override {
        auto &child = process(component);
        if (child && child->waitable()) {
            return Result<void>::failure(
                {ErrorCode::invalid_argument, "A supervised component is already running.",
                 "Collect or stop the current exact child before launching a replacement."});
        }

        ChildLaunch launch;
        auto childEnvironment = environment_;
        std::erase_if(childEnvironment.entries, [](const std::string &entry) {
            return environmentEntryNamed(entry, foundation::sessionReadinessDescriptorEnvironment);
        });
        int readinessDescriptor = -1;
        if (component == ComponentRole::settingsd) {
            launch.executable = options_.settingsdExecutable;
            launch.argv = {"prismdrake-settingsd", "--foreground"};
            if (safeMode) {
                launch.argv.emplace_back("--safe-mode");
            }
        } else if (component == ComponentRole::notifyd) {
            launch.executable = options_.notifydExecutable;
            launch.argv = {"prismdrake-notifyd", "--foreground"};
        } else {
            closeDescriptor(shell_readiness_descriptor_);
            auto readiness = createShellReadinessDescriptor();
            if (!readiness) {
                return Result<void>::failure(readiness.error());
            }
            readinessDescriptor = readiness.value();
            launch.executable = options_.shellExecutable;
            launch.argv = {"prismdrake-shell"};
            launch.inheritedDescriptor = readinessDescriptor;
            childEnvironment.entries.emplace_back(
                std::string{foundation::sessionReadinessDescriptorEnvironment} + '=' +
                std::to_string(readinessDescriptor));
        }

        auto launched = launchChildProcess(launch, childEnvironment);
        if (!launched) {
            closeDescriptor(readinessDescriptor);
            return Result<void>::failure(launched.error());
        }
        child.reset();
        child.emplace(std::move(launched).value());
        if (component == ComponentRole::shell) {
            shell_readiness_descriptor_ = std::exchange(readinessDescriptor, -1);
        }
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
            if (component == ComponentRole::shell) {
                closeDescriptor(shell_readiness_descriptor_);
            }
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

    Result<void> waitForNotificationReadiness(std::chrono::milliseconds timeout) override {
        const auto &child = process(ComponentRole::notifyd);
        if (!child || !child->waitable()) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "The notification readiness wait is invalid.",
                                          "Wait only for the current exact notification child."});
        }
        return probeNotificationReadiness(child->pid(), timeout);
    }

    Result<void> waitForShellReadiness(std::chrono::milliseconds timeout) override {
        if (timeout <= std::chrono::milliseconds::zero() ||
            timeout > sessionShellReadinessTimeout || shell_readiness_descriptor_ < 0 ||
            !running(ComponentRole::shell)) {
            return Result<void>::failure(
                {ErrorCode::invalid_argument, "The shell readiness wait is invalid.",
                 "Wait only for the current exact shell child within the bounded deadline."});
        }

        pollfd state{shell_readiness_descriptor_, POLLIN, 0};
        int result = -1;
        do {
            result = ::poll(&state, 1U, static_cast<int>(timeout.count()));
        } while (result < 0 && errno == EINTR && !shutdownRequested());
        if (result == 0 || (result < 0 && errno == EINTR)) {
            return Result<void>::failure(
                {ErrorCode::not_found, "The shell is not ready yet.",
                 "Continue the bounded readiness wait for the current shell child."});
        }
        if (result < 0) {
            closeDescriptor(shell_readiness_descriptor_);
            return Result<void>::failure(
                {ErrorCode::io_error, "The shell readiness channel could not be monitored.",
                 "Restart the exact shell child through bounded recovery."});
        }

        if ((state.revents & POLLIN) != 0) {
            std::uint64_t message = 0U;
            ssize_t received = -1;
            do {
                received = ::read(shell_readiness_descriptor_, &message, sizeof(message));
            } while (received < 0 && errno == EINTR);
            closeDescriptor(shell_readiness_descriptor_);
            if (received == static_cast<ssize_t>(sizeof(message)) &&
                message == foundation::sessionReadinessMessage) {
                return Result<void>::success();
            }
            if (received > 0) {
                return Result<void>::failure(shellReadinessContractFailure());
            }
            return Result<void>::failure(
                {ErrorCode::io_error, "The shell readiness channel closed before readiness.",
                 "Restart the exact shell child through bounded recovery."});
        }

        closeDescriptor(shell_readiness_descriptor_);
        return Result<void>::failure({ErrorCode::io_error,
                                      "The shell readiness channel closed before readiness.",
                                      "Restart the exact shell child through bounded recovery."});
    }

    Result<void> ownSessionControl(ExactChildShutdownPort &shutdownPort) override {
        if (session_bus_ || session_control_) {
            return Result<void>::failure(sessionControlFailure(ErrorCode::invalid_argument));
        }
        if (sd_bus_open_user(session_bus_.put()) < 0 ||
            sd_bus_set_exit_on_disconnect(session_bus_.get(), 0) < 0) {
            session_bus_.reset();
            return Result<void>::failure(sessionControlFailure());
        }
        auto service = std::make_unique<SessionControlService>(shutdownPort);
        auto attached = service->attach(session_bus_.get());
        if (!attached) {
            session_bus_.reset();
            return attached;
        }
        const int owned = sd_bus_request_name(session_bus_.get(), "org.prismdrake.Session1", 0);
        if (owned < 0) {
            session_bus_.reset();
            return Result<void>::failure(sessionControlFailure(
                owned == -EEXIST ? ErrorCode::invalid_environment : ErrorCode::io_error));
        }
        session_control_ = std::move(service);
        return Result<void>::success();
    }

    Result<void> dispatchSessionControl(std::size_t maximumMessages) override {
        if (!session_bus_ || !session_control_ || maximumMessages == 0U) {
            return Result<void>::failure(sessionControlFailure(ErrorCode::invalid_argument));
        }
        for (std::size_t count = 0U; count < maximumMessages && !shutdownRequested(); ++count) {
            const int processed = sd_bus_process(session_bus_.get(), nullptr);
            if (processed < 0) {
                return Result<void>::failure(sessionControlFailure());
            }
            if (processed == 0) {
                break;
            }
        }
        return Result<void>::success();
    }

    Result<void> waitForActivity() override {
        if (!session_bus_ || !session_control_ || sessionActivityReadDescriptor < 0) {
            return Result<void>::failure(activityWaitFailure());
        }

        const int busDescriptor = sd_bus_get_fd(session_bus_.get());
        const int busEvents = sd_bus_get_events(session_bus_.get());
        std::uint64_t busDeadline = UINT64_MAX;
        if (busDescriptor < 0 || busEvents < 0 ||
            sd_bus_get_timeout(session_bus_.get(), &busDeadline) < 0) {
            return Result<void>::failure(activityWaitFailure());
        }

        timespec timeout{};
        const timespec *timeoutPointer = nullptr;
        if (busDeadline != UINT64_MAX) {
            const auto monotonicNow = std::chrono::duration_cast<std::chrono::microseconds>(
                                          MonotonicClock::Clock::now().time_since_epoch())
                                          .count();
            const auto nowMicroseconds = monotonicNow > 0
                                             ? static_cast<std::uint64_t>(monotonicNow)
                                             : std::uint64_t{0U};
            const auto remainingMicroseconds =
                busDeadline > nowMicroseconds ? busDeadline - nowMicroseconds : 0U;
            timeout.tv_sec = static_cast<time_t>(remainingMicroseconds / 1'000'000U);
            timeout.tv_nsec =
                static_cast<long>((remainingMicroseconds % 1'000'000U) * 1'000U);
            timeoutPointer = &timeout;
        }

        std::array descriptors{
            pollfd{busDescriptor, static_cast<short>(busEvents), 0},
            pollfd{sessionActivityReadDescriptor, POLLIN, 0},
        };
        const int result = ::ppoll(descriptors.data(), descriptors.size(), timeoutPointer, nullptr);
        if (result < 0) {
            // Signal delivery is itself activity. The controller immediately checks shutdown and
            // every exact child, so a spurious EINTR is safe and simply returns to this wait.
            return errno == EINTR ? Result<void>::success()
                                  : Result<void>::failure(activityWaitFailure());
        }
        if ((descriptors[0].revents & POLLNVAL) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return Result<void>::failure(activityWaitFailure());
        }

        if ((descriptors[1].revents & POLLIN) != 0) {
            std::array<std::uint8_t, 64U> pending{};
            for (;;) {
                const ssize_t received =
                    ::read(sessionActivityReadDescriptor, pending.data(), pending.size());
                if (received > 0) {
                    continue;
                }
                if (received < 0 && errno == EINTR) {
                    continue;
                }
                if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                return Result<void>::failure(activityWaitFailure());
            }
        }
        return Result<void>::success();
    }

    void observeLogoutShutdown(std::uint64_t transactionId,
                               ExactChildShutdownEvent event) noexcept override {
        if (session_control_) {
            static_cast<void>(session_control_->observeShutdownEvent(transactionId, event));
        }
    }

    void beginSessionControlStop() noexcept override {
        if (session_control_) {
            session_control_->beginServiceStop();
        }
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
        switch (component) {
        case ComponentRole::settingsd:
            return settingsd_;
        case ComponentRole::notifyd:
            return notifyd_;
        case ComponentRole::shell:
            return shell_;
        }
        return shell_;
    }
    [[nodiscard]] const std::optional<ChildProcess> &
    process(ComponentRole component) const noexcept {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsd_;
        case ComponentRole::notifyd:
            return notifyd_;
        case ComponentRole::shell:
            return shell_;
        }
        return shell_;
    }

    SessionRuntimeOptions options_;
    PreparedSessionEnvironment environment_;
    SessionRuntime *runtime_;
    std::optional<ChildProcess> settingsd_;
    std::optional<ChildProcess> notifyd_;
    std::optional<ChildProcess> shell_;
    int shell_readiness_descriptor_{-1};
    ipc::sdbus::Bus session_bus_;
    std::unique_ptr<SessionControlService> session_control_;
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
        auto notification = startUntilStable(ComponentRole::notifyd);
        if (!notification) {
            static_cast<void>(shutdownAll());
            return Result<SessionTermination>::failure(notification.error());
        }
        if (notification.value() == Progress::shutdown_requested) {
            auto stopped = shutdownAll();
            return stopped
                       ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
        }
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

    auto control = platform_->ownSessionControl(*this);
    if (!control) {
        static_cast<void>(shutdownAll());
        return Result<SessionTermination>::failure(control.error());
    }

    auto ready = platform_->markReady();
    if (!ready) {
        static_cast<void>(shutdownAll());
        return Result<SessionTermination>::failure(ready.error());
    }

    for (;;) {
        auto controlDispatch =
            platform_->dispatchSessionControl(maximumSessionControlMessagesPerTick);
        if (!controlDispatch) {
            platform_->beginSessionControlStop();
            static_cast<void>(shutdownAll());
            return Result<SessionTermination>::failure(controlDispatch.error());
        }
        if (pending_logout_transaction_) {
            const auto transactionId = *pending_logout_transaction_;
            auto stopped = shutdownForLogout(transactionId);
            pending_logout_transaction_.reset();
            return stopped ? Result<SessionTermination>::success(SessionTermination::clean)
                           : Result<SessionTermination>::failure(stopped.error());
        }

        if (platform_->shutdownRequested()) {
            platform_->beginSessionControlStop();
            platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
                DiagnosticComponent::session, DiagnosticSeverity::info,
                DiagnosticEventId::operation_cancelled, std::nullopt, std::nullopt,
                DiagnosticRecovery::none));
            auto stopped = shutdownAll();
            return stopped
                       ? Result<SessionTermination>::success(SessionTermination::shutdown_requested)
                       : Result<SessionTermination>::failure(stopped.error());
        }

        for (const auto component :
             {ComponentRole::shell, ComponentRole::notifyd, ComponentRole::settingsd}) {
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
            if (progress.value() == Progress::degraded) {
                continue;
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
        auto activity = platform_->waitForActivity();
        if (!activity) {
            platform_->beginSessionControlStop();
            static_cast<void>(shutdownAll());
            return Result<SessionTermination>::failure(activityWaitFailure());
        }
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

        const auto readinessTimeout =
            component == ComponentRole::settingsd ? sessionSettingsReadinessTimeout
            : component == ComponentRole::notifyd ? sessionNotificationReadinessTimeout
                                                  : sessionShellReadinessTimeout;
        const auto readinessAttemptTimeout =
            component == ComponentRole::settingsd ? sessionSettingsReadinessAttemptTimeout
            : component == ComponentRole::notifyd ? sessionNotificationReadinessAttemptTimeout
                                                  : sessionShellReadinessAttemptTimeout;
        const auto readinessDeadline = platform_->now() + readinessTimeout;
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

            const auto attemptTimeout =
                waitSliceUntil(readinessDeadline, platform_->now(), readinessAttemptTimeout);
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

            Result<void> ready = component == ComponentRole::settingsd
                                     ? platform_->waitForSettingsReadiness(attemptTimeout)
                                 : component == ComponentRole::notifyd
                                     ? platform_->waitForNotificationReadiness(attemptTimeout)
                                     : platform_->waitForShellReadiness(attemptTimeout);
            if (ready) {
                return Result<Progress>::success(Progress::running);
            }
            if (ready.error().code != ErrorCode::not_found) {
                auto stopped = stopComponent(component);
                if (!stopped) {
                    return Result<Progress>::failure(stopped.error());
                }
                if (permanentReadinessError(ready.error().code)) {
                    if (component == ComponentRole::notifyd) {
                        publishComponentFailure(component, DiagnosticRecovery::use_fallback);
                        return Result<Progress>::success(Progress::degraded);
                    }
                    return Result<Progress>::failure(component == ComponentRole::settingsd
                                                         ? settingsReadinessContractFailure()
                                                         : shellReadinessContractFailure());
                }
                const auto launchObservation = launchedAt.value_or(platform_->now());
                launchedAt.reset();
                auto recovery = recover(component, ChildExitReason::failure, launchObservation);
                if (!recovery || recovery.value() != Progress::running) {
                    return recovery;
                }
                break;
            }

            if (component != ComponentRole::shell) {
                const auto retryDelay = waitSliceUntil(readinessDeadline, platform_->now());
                if (retryDelay > std::chrono::milliseconds::zero() &&
                    !waitInterruptibly(retryDelay)) {
                    auto stopped = stopComponent(component);
                    return stopped ? Result<Progress>::success(Progress::shutdown_requested)
                                   : Result<Progress>::failure(stopped.error());
                }
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
    case RecoveryAction::degrade_component:
        platform_->publishDiagnostic(foundation::makeDiagnosticEvent(
            DiagnosticComponent::notifyd, DiagnosticSeverity::warning,
            DiagnosticEventId::dependency_unavailable, std::nullopt, std::nullopt,
            DiagnosticRecovery::use_fallback));
        return Result<Progress>::success(Progress::degraded);
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
    auto stopped = stopAllChildren();
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
    auto notification = startUntilStable(ComponentRole::notifyd);
    if (!notification) {
        return Result<Progress>::failure(notification.error());
    }
    if (notification.value() == Progress::shutdown_requested) {
        return notification;
    }
    if (notification.value() != Progress::degraded && notification.value() != Progress::running) {
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
    platform_->beginSessionControlStop();
    return stopAllChildren();
}

Result<void> SessionSupervisor::stopAllChildren() {
    std::optional<Error> firstError;
    for (const auto component :
         {ComponentRole::shell, ComponentRole::notifyd, ComponentRole::settingsd}) {
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

Result<void> SessionSupervisor::shutdownForLogout(std::uint64_t transactionId) {
    platform_->beginSessionControlStop();
    std::optional<Error> firstError;
    constexpr std::array sequence{
        std::pair{ComponentRole::shell, ExactChildShutdownEvent::shell_stopped},
        std::pair{ComponentRole::notifyd, ExactChildShutdownEvent::notifyd_stopped},
        std::pair{ComponentRole::settingsd, ExactChildShutdownEvent::settingsd_stopped},
    };
    for (const auto &[component, event] : sequence) {
        auto stopped = stopComponent(component);
        if (!stopped) {
            if (!firstError) {
                firstError = stopped.error();
            }
            platform_->observeLogoutShutdown(transactionId, ExactChildShutdownEvent::failed);
            continue;
        }
        platform_->observeLogoutShutdown(transactionId, event);
    }
    return firstError ? Result<void>::failure(*firstError) : Result<void>::success();
}

bool SessionSupervisor::beginExactChildShutdown(std::uint64_t transactionId) {
    if (transactionId == 0U || pending_logout_transaction_) {
        return false;
    }
    pending_logout_transaction_ = transactionId;
    return true;
}

bool SessionSupervisor::waitInterruptibly(std::chrono::milliseconds duration) {
    auto remaining = duration;
    while (remaining > std::chrono::milliseconds::zero()) {
        if (platform_->shutdownRequested()) {
            return false;
        }
        const auto slice = std::min(sessionInterruptibleWaitSlice, remaining);
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
    switch (component) {
    case ComponentRole::settingsd:
        return settingsd_launched_at_;
    case ComponentRole::notifyd:
        return notifyd_launched_at_;
    case ComponentRole::shell:
        return shell_launched_at_;
    }
    return shell_launched_at_;
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
    if (sessionActivityReadDescriptor < 0 || sessionActivityWriteDescriptor < 0) {
        std::array<int, 2U> descriptors{-1, -1};
        if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
            return Result<void>::failure(
                {ErrorCode::io_error, "The session activity channel could not be created.",
                 "Retry the session after checking process descriptor limits."});
        }
        sessionActivityReadDescriptor = descriptors[0];
        sessionActivityWriteDescriptor = descriptors[1];
    }
    struct sigaction action = {};
    action.sa_handler = wakeSessionSupervisor;
    action.sa_flags = SA_NOCLDSTOP;
    if (::sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGINT, &action, nullptr) != 0 ||
        ::sigaction(SIGTERM, &action, nullptr) != 0 ||
        ::sigaction(SIGCHLD, &action, nullptr) != 0) {
        return Result<void>::failure(
            {ErrorCode::io_error, "Session shutdown handlers could not be installed.",
             "Retry the development session in a valid POSIX process environment."});
    }
    return Result<void>::success();
}

} // namespace prismdrake::session
