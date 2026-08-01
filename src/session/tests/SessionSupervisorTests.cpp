#include "SessionSupervisor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace prismdrake::session {
namespace {

using namespace std::chrono_literals;
using foundation::DiagnosticEvent;
using foundation::DiagnosticEventId;
using foundation::ErrorCode;
using foundation::MonotonicClock;
using foundation::Result;

struct LaunchRecord final {
    ComponentRole component;
    bool safeMode;
};

class RuntimeFixture final {
  public:
    RuntimeFixture() {
        std::string pattern = "/tmp/prismdrake-supervisor-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create the supervisor runtime fixture."};
        }
        root_ = created;
        base_ = root_ / "runtime";
        prismdrake_ = base_ / "prismdrake";
        std::filesystem::create_directories(prismdrake_);
        std::filesystem::permissions(base_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::filesystem::permissions(prismdrake_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    ~RuntimeFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    RuntimeFixture(const RuntimeFixture &) = delete;
    RuntimeFixture &operator=(const RuntimeFixture &) = delete;

    [[nodiscard]] foundation::XdgPaths paths() const { return {{}, {}, {}, {}, prismdrake_}; }

  private:
    std::filesystem::path root_;
    std::filesystem::path base_;
    std::filesystem::path prismdrake_;
};

[[nodiscard]] std::filesystem::path productionChildFixture() {
    const char *path = std::getenv("PRISMDRAKE_SESSION_SUPERVISOR_CHILD_FIXTURE");
    if (path == nullptr || *path == '\0') {
        throw std::runtime_error{"The supervisor child fixture path is unavailable."};
    }
    return path;
}

[[nodiscard]] PreparedSessionEnvironment
productionChildEnvironment(std::string_view readinessMode = {}) {
    PreparedSessionEnvironment environment{
        {"DISPLAY=:77", "XDG_RUNTIME_DIR=/tmp", "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus",
         "SUPERVISOR_FIXTURE=exact-value", "XDG_CURRENT_DESKTOP=Prismdrake",
         "XDG_SESSION_DESKTOP=prismdrake", "DESKTOP_SESSION=prismdrake"},
        ":77",
        "/tmp",
        "unix:path=/tmp/bus"};
    if (!readinessMode.empty()) {
        environment.entries.emplace_back("SUPERVISOR_FIXTURE_READY_MODE=" +
                                         std::string{readinessMode});
    }
    return environment;
}

[[nodiscard]] ChildExitStatus waitForProductionChild(SessionSupervisorPlatform &platform,
                                                     ComponentRole component) {
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt) {
        auto status = platform.poll(component);
        EXPECT_TRUE(status);
        if (!status) {
            return {ChildExitKind::exited, 125, false};
        }
        if (status.value()) {
            return *status.value();
        }
        std::this_thread::sleep_for(1ms);
    }
    ADD_FAILURE() << "production child did not exit within the test bound";
    return {ChildExitKind::exited, 124, false};
}

class FakePlatform final : public SessionSupervisorPlatform {
  public:
    Result<void> launch(ComponentRole component, bool safeMode) override {
        launches.push_back({component, safeMode});
        auto &failures = component == ComponentRole::settingsd ? settingsLaunchFailures
                         : component == ComponentRole::notifyd ? notifyLaunchFailures
                                                               : shellLaunchFailures;
        if (failures > 0U) {
            --failures;
            return Result<void>::failure({ErrorCode::io_error, "private-component-path-sentinel",
                                          "private-component-recovery-sentinel"});
        }
        terminateRequested(component) = false;
        killRequested(component) = false;
        runningState(component) = true;
        if (shutdownAfterLaunches && launches.size() >= *shutdownAfterLaunches) {
            requestShutdown = true;
        }
        return Result<void>::success();
    }

    Result<std::optional<ChildExitStatus>> poll(ComponentRole component) override {
        polls.push_back(component);
        if (pollFailures > 0U && (!pollFailuresAfterReadyOnly || readyMarks > 0U)) {
            --pollFailures;
            return Result<std::optional<ChildExitStatus>>::failure(
                {ErrorCode::io_error, "private-poll-failure", "private-poll-recovery"});
        }
        auto &exits = component == ComponentRole::settingsd ? settingsExits
                      : component == ComponentRole::notifyd ? notifyExits
                                                            : shellExits;
        const bool exitEligible =
            (component == ComponentRole::settingsd &&
             (!settingsExitsAfterReadyOnly || readyMarks > 0U)) ||
            (component == ComponentRole::notifyd &&
             (!notifyExitsAfterReadyOnly || readyMarks > 0U)) ||
            (component == ComponentRole::shell && (!shellExitsAfterReadyOnly || readyMarks > 0U));
        if (exitEligible && !exits.empty()) {
            auto status = exits.front();
            exits.pop_front();
            runningState(component) = false;
            return Result<std::optional<ChildExitStatus>>::success(status);
        }
        if (terminateRequested(component) && !ignoreTerminate(component)) {
            runningState(component) = false;
            return Result<std::optional<ChildExitStatus>>::success(
                ChildExitStatus{ChildExitKind::signaled, SIGTERM, false});
        }
        if (killRequested(component)) {
            runningState(component) = false;
            return Result<std::optional<ChildExitStatus>>::success(
                ChildExitStatus{ChildExitKind::signaled, SIGKILL, false});
        }
        return Result<std::optional<ChildExitStatus>>::success(std::nullopt);
    }

    bool running(ComponentRole component) const noexcept override {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsRunning;
        case ComponentRole::notifyd:
            return notifyRunning;
        case ComponentRole::shell:
            return shellRunning;
        }
        return false;
    }

    Result<void> requestTerminate(ComponentRole component) override {
        stopOrder.push_back(component);
        terminateRequested(component) = true;
        return Result<void>::success();
    }

    Result<void> forceKill(ComponentRole component) override {
        forced.push_back(component);
        killRequested(component) = true;
        return Result<void>::success();
    }

    Result<void> waitForSettingsReadiness(std::chrono::milliseconds timeout) override {
        readinessTimeouts.push_back(timeout);
        if (shutdownAfterReadinessCalls &&
            readinessTimeouts.size() >= *shutdownAfterReadinessCalls) {
            requestShutdown = true;
        }
        const auto settingsLaunches = static_cast<std::size_t>(
            std::count_if(launches.begin(), launches.end(), [](const auto &launch) {
                return launch.component == ComponentRole::settingsd;
            }));
        if (settingsLaunches <= readinessUnavailableThroughSettingsLaunch) {
            return Result<void>::failure(
                {ErrorCode::not_found, "fixture readiness unavailable", "retry fixture"});
        }
        if (settingsReadinessFailures > 0U) {
            --settingsReadinessFailures;
            return Result<void>::failure(
                {settingsReadinessFailureCode, "fixture readiness failure", "retry fixture"});
        }
        return Result<void>::success();
    }

    Result<void> waitForNotificationReadiness(std::chrono::milliseconds timeout) override {
        notificationReadinessTimeouts.push_back(timeout);
        const auto launchesForNotifyd = static_cast<std::size_t>(
            std::count_if(launches.begin(), launches.end(), [](const auto &launch) {
                return launch.component == ComponentRole::notifyd;
            }));
        if (launchesForNotifyd <= readinessUnavailableThroughNotifyLaunch) {
            currentTime += timeout;
            return Result<void>::failure(
                {ErrorCode::not_found, "fixture notification unavailable", "retry fixture"});
        }
        if (notificationReadinessFailures > 0U) {
            --notificationReadinessFailures;
            return Result<void>::failure({notificationReadinessFailureCode,
                                          "fixture notification readiness failure",
                                          "retry fixture"});
        }
        return Result<void>::success();
    }

    Result<void> waitForShellReadiness(std::chrono::milliseconds timeout) override {
        shellReadinessTimeouts.push_back(timeout);
        if (shutdownAfterShellReadinessCalls &&
            shellReadinessTimeouts.size() >= *shutdownAfterShellReadinessCalls) {
            requestShutdown = true;
        }
        const auto shellLaunches = static_cast<std::size_t>(
            std::count_if(launches.begin(), launches.end(), [](const auto &launch) {
                return launch.component == ComponentRole::shell;
            }));
        if (shellLaunches <= readinessUnavailableThroughShellLaunch) {
            currentTime += timeout;
            return Result<void>::failure(
                {ErrorCode::not_found, "fixture shell readiness unavailable", "retry fixture"});
        }
        if (shellReadinessFailures > 0U) {
            --shellReadinessFailures;
            return Result<void>::failure(
                {shellReadinessFailureCode, "fixture shell readiness failure", "retry fixture"});
        }
        return Result<void>::success();
    }

    Result<void> markReady() override {
        ++readyMarks;
        if (shutdownOnReady) {
            requestShutdown = true;
        }
        return Result<void>::success();
    }

    Result<void> ownSessionControl(ExactChildShutdownPort &shutdownPort) override {
        shutdownPort_ = &shutdownPort;
        ++sessionControlOwners;
        return sessionControlOwnerFailure
                   ? Result<void>::failure({ErrorCode::invalid_environment,
                                            "fixture session owner conflict", "retry fixture"})
                   : Result<void>::success();
    }

    Result<void> dispatchSessionControl(std::size_t maximumMessages) override {
        sessionControlDispatchLimits.push_back(maximumMessages);
        ++sessionControlDispatches;
        if (logoutOnNextDispatch && shutdownPort_ != nullptr) {
            logoutOnNextDispatch = false;
            static_cast<void>(shutdownPort_->beginExactChildShutdown(
                logoutTransactionId, foundation::MonotonicClock::TimePoint::max()));
        }
        return Result<void>::success();
    }

    Result<void> waitForActivity() override {
        ++activityWaits;
        if (activityWaitFailures > 0U) {
            --activityWaitFailures;
            return Result<void>::failure(
                {ErrorCode::io_error, "private-activity-failure", "private-activity-recovery"});
        }
        if (shellExitOnActivityWait) {
            shellExits.push_back(*shellExitOnActivityWait);
            shellExitOnActivityWait.reset();
        }
        if (logoutOnActivityWait) {
            logoutOnActivityWait = false;
            logoutOnNextDispatch = true;
        }
        if (shutdownAfterActivityWaits && activityWaits >= *shutdownAfterActivityWaits) {
            requestShutdown = true;
        }
        return Result<void>::success();
    }

    void observeLogoutShutdown(std::uint64_t transactionId,
                               ExactChildShutdownEvent event) noexcept override {
        logoutEvents.emplace_back(transactionId, event);
    }

    void beginSessionControlStop() noexcept override { ++sessionControlStops; }

    Result<void> markSafeMode() override {
        ++safeModeMarks;
        return Result<void>::success();
    }

    MonotonicClock::TimePoint now() const noexcept override { return currentTime; }
    bool shutdownRequested() const noexcept override { return requestShutdown; }

    void waitFor(std::chrono::milliseconds duration) override {
        waits.push_back(duration);
        currentTime += duration;
        if (shutdownAfterWaits && waits.size() >= *shutdownAfterWaits) {
            requestShutdown = true;
        }
    }

    void publishDiagnostic(const DiagnosticEvent &event) override {
        diagnostics.push_back(event.eventId());
    }

    std::size_t settingsLaunchFailures{0U};
    std::size_t notifyLaunchFailures{0U};
    std::size_t shellLaunchFailures{0U};
    std::size_t settingsReadinessFailures{0U};
    std::size_t shellReadinessFailures{0U};
    std::size_t notificationReadinessFailures{0U};
    std::size_t readinessUnavailableThroughSettingsLaunch{0U};
    std::size_t readinessUnavailableThroughShellLaunch{0U};
    std::size_t readinessUnavailableThroughNotifyLaunch{0U};
    std::size_t pollFailures{0U};
    ErrorCode settingsReadinessFailureCode{ErrorCode::not_found};
    ErrorCode shellReadinessFailureCode{ErrorCode::not_found};
    ErrorCode notificationReadinessFailureCode{ErrorCode::not_found};
    bool pollFailuresAfterReadyOnly{false};
    bool settingsExitsAfterReadyOnly{false};
    bool shellExitsAfterReadyOnly{true};
    bool notifyExitsAfterReadyOnly{true};
    bool settingsRunning{false};
    bool shellRunning{false};
    bool notifyRunning{false};
    bool settingsIgnoreTerminate{false};
    bool shellIgnoreTerminate{false};
    bool notifyIgnoreTerminate{false};
    bool settingsTerminateRequested{false};
    bool shellTerminateRequested{false};
    bool notifyTerminateRequested{false};
    bool settingsKillRequested{false};
    bool shellKillRequested{false};
    bool notifyKillRequested{false};
    bool requestShutdown{false};
    bool shutdownOnReady{false};
    std::optional<std::size_t> shutdownAfterLaunches;
    std::optional<std::size_t> shutdownAfterReadinessCalls;
    std::optional<std::size_t> shutdownAfterShellReadinessCalls;
    std::optional<std::size_t> shutdownAfterWaits;
    std::optional<std::size_t> shutdownAfterActivityWaits;
    std::optional<ChildExitStatus> shellExitOnActivityWait;
    std::deque<ChildExitStatus> settingsExits;
    std::deque<ChildExitStatus> shellExits;
    std::deque<ChildExitStatus> notifyExits;
    MonotonicClock::TimePoint currentTime{};
    std::vector<LaunchRecord> launches;
    std::vector<ComponentRole> polls;
    std::vector<ComponentRole> stopOrder;
    std::vector<ComponentRole> forced;
    std::vector<std::chrono::milliseconds> waits;
    std::vector<std::chrono::milliseconds> readinessTimeouts;
    std::vector<std::chrono::milliseconds> shellReadinessTimeouts;
    std::vector<std::chrono::milliseconds> notificationReadinessTimeouts;
    std::vector<DiagnosticEventId> diagnostics;
    std::size_t readyMarks{0U};
    std::size_t safeModeMarks{0U};
    std::size_t sessionControlOwners{0U};
    std::size_t sessionControlDispatches{0U};
    std::size_t activityWaits{0U};
    std::size_t activityWaitFailures{0U};
    std::vector<std::size_t> sessionControlDispatchLimits;
    std::size_t sessionControlStops{0U};
    bool sessionControlOwnerFailure{false};
    bool logoutOnNextDispatch{false};
    bool logoutOnActivityWait{false};
    std::uint64_t logoutTransactionId{17U};
    std::vector<std::pair<std::uint64_t, ExactChildShutdownEvent>> logoutEvents;

  private:
    [[nodiscard]] bool &runningState(ComponentRole component) noexcept {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsRunning;
        case ComponentRole::notifyd:
            return notifyRunning;
        case ComponentRole::shell:
            return shellRunning;
        }
        return shellRunning;
    }
    [[nodiscard]] bool &terminateRequested(ComponentRole component) noexcept {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsTerminateRequested;
        case ComponentRole::notifyd:
            return notifyTerminateRequested;
        case ComponentRole::shell:
            return shellTerminateRequested;
        }
        return shellTerminateRequested;
    }
    [[nodiscard]] bool &killRequested(ComponentRole component) noexcept {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsKillRequested;
        case ComponentRole::notifyd:
            return notifyKillRequested;
        case ComponentRole::shell:
            return shellKillRequested;
        }
        return shellKillRequested;
    }
    [[nodiscard]] bool ignoreTerminate(ComponentRole component) const noexcept {
        switch (component) {
        case ComponentRole::settingsd:
            return settingsIgnoreTerminate;
        case ComponentRole::notifyd:
            return notifyIgnoreTerminate;
        case ComponentRole::shell:
            return shellIgnoreTerminate;
        }
        return false;
    }

    ExactChildShutdownPort *shutdownPort_ = nullptr;
};

[[nodiscard]] SessionSupervisor
supervisor(FakePlatform &platform, SessionRecoveryConfig config = pd2SessionRecoveryConfig()) {
    auto created = SessionSupervisor::create(platform, config);
    EXPECT_TRUE(created);
    return std::move(created).value();
}

TEST(SessionSupervisorTest, StartsInDependencyOrderMarksReadyAndTreatsCleanShellExitAsLogout) {
    FakePlatform platform;
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
    EXPECT_FALSE(platform.launches[0].safeMode);
    EXPECT_EQ(platform.readinessTimeouts,
              (std::vector<std::chrono::milliseconds>{sessionSettingsReadinessAttemptTimeout}));
    EXPECT_EQ(platform.shellReadinessTimeouts,
              (std::vector<std::chrono::milliseconds>{sessionShellReadinessAttemptTimeout}));
    EXPECT_EQ(platform.readyMarks, 1U);
    ASSERT_FALSE(platform.sessionControlDispatchLimits.empty());
    EXPECT_TRUE(std::ranges::all_of(platform.sessionControlDispatchLimits, [](const auto limit) {
        return limit == maximumSessionControlMessagesPerTick;
    }));
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::notifyd, ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, ShellReadinessTimeoutRestartsOnlyShellWithinTheBound) {
    FakePlatform platform;
    platform.readinessUnavailableThroughShellLaunch = 1U;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    ASSERT_EQ(platform.launches.size(), 4U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
    EXPECT_EQ(platform.launches[3].component, ComponentRole::shell);
    ASSERT_GT(platform.shellReadinessTimeouts.size(), 1U);
    EXPECT_LE(*std::max_element(platform.shellReadinessTimeouts.begin(),
                                platform.shellReadinessTimeouts.end()),
              sessionShellReadinessAttemptTimeout);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.end(), 0ms), 250ms);
    EXPECT_EQ(
        std::count(platform.stopOrder.begin(), platform.stopOrder.end(), ComponentRole::settingsd),
        1);
}

TEST(SessionSupervisorTest, ShellCrashBeforeReadinessRecoversWithoutMarkingTheSessionReady) {
    FakePlatform platform;
    platform.shellExitsAfterReadyOnly = false;
    platform.shellExits.push_back({ChildExitKind::exited, 17, false});
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(
        std::count_if(platform.launches.begin(), platform.launches.end(),
                      [](const auto &launch) { return launch.component == ComponentRole::shell; }),
        2);
    EXPECT_EQ(platform.shellReadinessTimeouts.size(), 1U);
    EXPECT_EQ(platform.readyMarks, 1U);
}

TEST(SessionSupervisorTest, ShutdownInterruptsShellReadinessAndStopsInReverseOrder) {
    FakePlatform platform;
    platform.readinessUnavailableThroughShellLaunch = 10U;
    platform.shutdownAfterShellReadinessCalls = 1U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.shellReadinessTimeouts.size(), 1U);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, InvalidShellReadinessIsTerminalAndRedacted) {
    FakePlatform platform;
    platform.shellReadinessFailures = 1U;
    platform.shellReadinessFailureCode = ErrorCode::validation_error;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::validation_error);
    EXPECT_EQ(outcome.error().message.find("fixture"), std::string::npos);
    EXPECT_EQ(outcome.error().recovery.find("fixture"), std::string::npos);
    EXPECT_EQ(platform.shellReadinessTimeouts.size(), 1U);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, RestartsCrashedShellWithBoundedBackoffWithoutStoppingSettings) {
    FakePlatform platform;
    platform.shellExits.push_back({ChildExitKind::exited, 17, false});
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 4U);
    EXPECT_EQ(platform.launches[3].component, ComponentRole::shell);
    ASSERT_GE(platform.waits.size(), 10U);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.begin() + 10, 0ms), 250ms);
    EXPECT_EQ(
        std::count(platform.stopOrder.begin(), platform.stopOrder.end(), ComponentRole::settingsd),
        1);
    EXPECT_NE(std::find(platform.diagnostics.begin(), platform.diagnostics.end(),
                        DiagnosticEventId::component_start_failed),
              platform.diagnostics.end());
}

TEST(SessionSupervisorTest, RestartsSettingsConservativelyAndReprobesWithoutStoppingShell) {
    FakePlatform platform;
    platform.settingsExits.push_back({ChildExitKind::exited, 19, false});
    platform.settingsExitsAfterReadyOnly = true;
    platform.shutdownAfterReadinessCalls = 2U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    ASSERT_EQ(platform.launches.size(), 4U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
    EXPECT_EQ(platform.launches[3].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.readinessTimeouts.size(), 2U);
    EXPECT_EQ(
        std::count(platform.stopOrder.begin(), platform.stopOrder.end(), ComponentRole::shell), 1);
}

TEST(SessionSupervisorTest, ExhaustionRestartsBothComponentsOnceInObservableSafeMode) {
    FakePlatform platform;
    platform.shellLaunchFailures = 4U;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.safeModeMarks, 1U);
    EXPECT_NE(std::find(platform.diagnostics.begin(), platform.diagnostics.end(),
                        DiagnosticEventId::fallback_selected),
              platform.diagnostics.end());
    const auto safeSettings =
        std::find_if(platform.launches.begin(), platform.launches.end(), [](const auto &launch) {
            return launch.component == ComponentRole::settingsd && launch.safeMode;
        });
    const auto safeShell =
        std::find_if(platform.launches.begin(), platform.launches.end(), [](const auto &launch) {
            return launch.component == ComponentRole::shell && launch.safeMode;
        });
    const auto safeNotifyd =
        std::find_if(platform.launches.begin(), platform.launches.end(), [](const auto &launch) {
            return launch.component == ComponentRole::notifyd && launch.safeMode;
        });
    EXPECT_NE(safeSettings, platform.launches.end());
    EXPECT_NE(safeNotifyd, platform.launches.end());
    EXPECT_NE(safeShell, platform.launches.end());
}

TEST(SessionSupervisorTest, MissingNotificationServiceDegradesWithoutBlockingShellReadiness) {
    FakePlatform platform;
    platform.readinessUnavailableThroughNotifyLaunch = 10U;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(std::count_if(
                  platform.launches.begin(), platform.launches.end(),
                  [](const auto &launch) { return launch.component == ComponentRole::notifyd; }),
              3);
    EXPECT_EQ(
        std::count_if(platform.launches.begin(), platform.launches.end(),
                      [](const auto &launch) { return launch.component == ComponentRole::shell; }),
        1);
    EXPECT_EQ(platform.readyMarks, 1U);
    EXPECT_NE(std::find(platform.diagnostics.begin(), platform.diagnostics.end(),
                        DiagnosticEventId::dependency_unavailable),
              platform.diagnostics.end());
}

TEST(SessionSupervisorTest, MissingNotificationExecutableDegradesAfterBoundedLaunchAttempts) {
    FakePlatform platform;
    platform.notifyLaunchFailures = 3U;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(std::count_if(
                  platform.launches.begin(), platform.launches.end(),
                  [](const auto &launch) { return launch.component == ComponentRole::notifyd; }),
              3);
    EXPECT_EQ(
        std::count_if(platform.launches.begin(), platform.launches.end(),
                      [](const auto &launch) { return launch.component == ComponentRole::shell; }),
        1);
    EXPECT_EQ(platform.readyMarks, 1U);
}

TEST(SessionSupervisorTest, NotificationNameConflictDegradesWithoutARestartLoop) {
    FakePlatform platform;
    platform.notificationReadinessFailures = 1U;
    platform.notificationReadinessFailureCode = ErrorCode::unsupported;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(std::count_if(
                  platform.launches.begin(), platform.launches.end(),
                  [](const auto &launch) { return launch.component == ComponentRole::notifyd; }),
              1);
    EXPECT_EQ(platform.readyMarks, 1U);
}

TEST(SessionSupervisorTest, NotificationCrashRestartsOnlyNotifydAndPreservesTheShell) {
    FakePlatform platform;
    platform.notifyExits.push_back({ChildExitKind::exited, 17, false});
    platform.shutdownAfterLaunches = 4U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(std::count_if(
                  platform.launches.begin(), platform.launches.end(),
                  [](const auto &launch) { return launch.component == ComponentRole::notifyd; }),
              2);
    EXPECT_EQ(
        std::count_if(platform.launches.begin(), platform.launches.end(),
                      [](const auto &launch) { return launch.component == ComponentRole::shell; }),
        1);
}

TEST(SessionSupervisorTest, ConfirmedLogoutStopsExactChildrenInCoordinatedReverseOrder) {
    FakePlatform platform;
    platform.logoutOnNextDispatch = true;
    auto controller = supervisor(platform);

    EXPECT_EQ(controller.beginExactChildShutdown(99U, platform.currentTime),
              ExactChildShutdownStart::timed_out);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
    EXPECT_EQ(platform.logoutEvents,
              (std::vector<std::pair<std::uint64_t, ExactChildShutdownEvent>>{
                  {platform.logoutTransactionId, ExactChildShutdownEvent::shell_stopped},
                  {platform.logoutTransactionId, ExactChildShutdownEvent::notifyd_stopped},
                  {platform.logoutTransactionId, ExactChildShutdownEvent::settingsd_stopped},
              }));
}

TEST(SessionSupervisorTest, StableSupervisionBlocksOnceForActivityWithoutCadencePolling) {
    FakePlatform platform;
    platform.shutdownAfterActivityWaits = 1U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.activityWaits, 1U);
    EXPECT_TRUE(platform.waits.empty());
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, ExactChildExitWakesActivityWaitAndIsReapedOnTheNextObservation) {
    FakePlatform platform;
    platform.shellExitOnActivityWait = ChildExitStatus{ChildExitKind::exited, 0, false};
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    EXPECT_EQ(platform.activityWaits, 1U);
    EXPECT_FALSE(platform.shellRunning);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::notifyd, ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, SessionBusWorkWakesActivityWaitAndPreservesLogoutOrdering) {
    FakePlatform platform;
    platform.logoutOnActivityWait = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    EXPECT_EQ(platform.activityWaits, 1U);
    EXPECT_GE(platform.sessionControlDispatches, 2U);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
    EXPECT_EQ(platform.logoutEvents,
              (std::vector<std::pair<std::uint64_t, ExactChildShutdownEvent>>{
                  {platform.logoutTransactionId, ExactChildShutdownEvent::shell_stopped},
                  {platform.logoutTransactionId, ExactChildShutdownEvent::notifyd_stopped},
                  {platform.logoutTransactionId, ExactChildShutdownEvent::settingsd_stopped},
              }));
}

TEST(SessionSupervisorTest, SpuriousActivityWakeReturnsSafelyToOneMoreBlockingWait) {
    FakePlatform platform;
    platform.shutdownAfterActivityWaits = 2U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.activityWaits, 2U);
    EXPECT_TRUE(platform.waits.empty());
}

TEST(SessionSupervisorTest, ActivityWaitFailureStopsExactChildrenAndRedactsPlatformDetails) {
    FakePlatform platform;
    platform.activityWaitFailures = 1U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::io_error);
    EXPECT_EQ(outcome.error().message.find("private-activity"), std::string::npos);
    EXPECT_EQ(outcome.error().recovery.find("private-activity"), std::string::npos);
    EXPECT_EQ(platform.activityWaits, 1U);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, SessionControlNameConflictStopsEveryExactChild) {
    FakePlatform platform;
    platform.sessionControlOwnerFailure = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, TransientReadinessFailureRetriesWithoutRestartingSettings) {
    FakePlatform platform;
    platform.settingsReadinessFailures = 1U;
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
    EXPECT_EQ(platform.readinessTimeouts.size(), 2U);
    EXPECT_EQ(platform.readinessTimeouts[0], sessionSettingsReadinessAttemptTimeout);
    EXPECT_EQ(platform.waits.front(), sessionInterruptibleWaitSlice);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.end(), 0ms),
              sessionInterruptibleWaitSlice);
}

TEST(SessionSupervisorTest, PermanentReadinessErrorStopsWithoutRetryingTheContract) {
    FakePlatform platform;
    platform.settingsReadinessFailures = 1U;
    platform.settingsReadinessFailureCode = ErrorCode::validation_error;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::validation_error);
    EXPECT_EQ(outcome.error().message.find("fixture"), std::string::npos);
    EXPECT_EQ(outcome.error().recovery.find("fixture"), std::string::npos);
    ASSERT_EQ(platform.launches.size(), 1U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.readinessTimeouts.size(), 1U);
    EXPECT_TRUE(platform.waits.empty());
}

TEST(SessionSupervisorTest, SettingsExitBeforeReadinessIsObservedWithoutWaitingForTheDeadline) {
    FakePlatform platform;
    platform.settingsExits.push_back({ChildExitKind::exited, 17, false});
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 4U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[3].component, ComponentRole::shell);
    EXPECT_EQ(platform.readinessTimeouts.size(), 1U);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.end(), 0ms), 500ms);
}

TEST(SessionSupervisorTest, ShutdownInterruptsReadinessRetryAndStopsSettingsWithoutRelaunch) {
    FakePlatform platform;
    platform.settingsReadinessFailures = 1U;
    platform.shutdownAfterWaits = 1U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    ASSERT_EQ(platform.launches.size(), 1U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.readinessTimeouts.size(), 1U);
    EXPECT_EQ(platform.waits,
              (std::vector<std::chrono::milliseconds>{sessionInterruptibleWaitSlice}));
    EXPECT_EQ(platform.stopOrder, (std::vector<ComponentRole>{ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, ReadinessDeadlineTriggersOneConservativeRestartWithinTheBound) {
    FakePlatform platform;
    platform.readinessUnavailableThroughSettingsLaunch = 1U;
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 4U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::notifyd);
    EXPECT_EQ(platform.launches[3].component, ComponentRole::shell);
    ASSERT_GT(platform.readinessTimeouts.size(), 1U);
    EXPECT_LE(
        *std::max_element(platform.readinessTimeouts.begin(), platform.readinessTimeouts.end()),
        sessionSettingsReadinessAttemptTimeout);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.end(), 0ms),
              sessionSettingsReadinessTimeout + 500ms);
}

TEST(SessionSupervisorTest, SignalRequestInterruptsBackoffAndUsesReverseBoundedShutdown) {
    FakePlatform platform;
    platform.shellLaunchFailures = 1U;
    platform.shutdownAfterWaits = 1U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::notifyd, ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, UnresponsiveChildrenAreKilledAndReportedInReverseOrder) {
    FakePlatform platform;
    platform.settingsIgnoreTerminate = true;
    platform.notifyIgnoreTerminate = true;
    platform.shellIgnoreTerminate = true;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
    EXPECT_EQ(platform.forced,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
    EXPECT_GE(std::count(platform.diagnostics.begin(), platform.diagnostics.end(),
                         DiagnosticEventId::operation_cancelled),
              3);
    EXPECT_LE(platform.currentTime.time_since_epoch(),
              3U * (sessionTerminateGrace + sessionKillReapGrace) + sessionInterruptibleWaitSlice);
}

TEST(SessionSupervisorTest, PollFailureStillStopsEveryOwnedChildInReverseOrder) {
    FakePlatform platform;
    platform.pollFailures = 1U;
    platform.pollFailuresAfterReadyOnly = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::io_error);
    EXPECT_EQ(outcome.error().message.find("private-poll"), std::string::npos);
    EXPECT_EQ(outcome.error().recovery.find("private-poll"), std::string::npos);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::notifyd,
                                          ComponentRole::settingsd}));
    EXPECT_FALSE(platform.shellRunning);
    EXPECT_FALSE(platform.notifyRunning);
    EXPECT_FALSE(platform.settingsRunning);
}

TEST(SessionSupervisorTest, SafeModeFailureIsTerminalAndNeverCreatesASecondRetryBudget) {
    FakePlatform platform;
    platform.shellLaunchFailures = 5U;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().code, ErrorCode::io_error);
    EXPECT_EQ(outcome.error().message.find("private-component"), std::string::npos);
    EXPECT_EQ(outcome.error().recovery.find("private-component"), std::string::npos);
    EXPECT_EQ(platform.safeModeMarks, 1U);
    EXPECT_EQ(std::count(platform.diagnostics.begin(), platform.diagnostics.end(),
                         DiagnosticEventId::component_restart_exhausted),
              1);
}

TEST(SessionSupervisorTest, ProductionBackendUsesExactArgvEnvironmentAndPidOwnership) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    const auto executable = productionChildFixture();
    auto platform = makeProductionSessionSupervisorPlatform(
        SessionRuntimeOptions{executable, executable, executable}, productionChildEnvironment(),
        runtime.value());

    ASSERT_TRUE(platform->launch(ComponentRole::shell, false));
    auto firstReady = platform->waitForShellReadiness(sessionShellReadinessAttemptTimeout);
    ASSERT_TRUE(firstReady) << firstReady.error().message;
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::shell),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::shell));

    ASSERT_TRUE(platform->launch(ComponentRole::shell, false));
    auto secondReady = platform->waitForShellReadiness(sessionShellReadinessAttemptTimeout);
    ASSERT_TRUE(secondReady) << secondReady.error().message;
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::shell),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::shell));

    ASSERT_TRUE(platform->launch(ComponentRole::settingsd, true));
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::settingsd),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::settingsd));
    ASSERT_TRUE(platform->launch(ComponentRole::notifyd, false));
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::notifyd),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::notifyd));
    ASSERT_TRUE(platform->markSafeMode());
    ASSERT_TRUE(platform->markReady());
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().safeModeMarkerPath()));
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().readyMarkerPath()));
    EXPECT_TRUE(runtime.value().cleanup());
}

TEST(SessionSupervisorTest, ProductionBackendRejectsClosedAndMalformedPerLaunchChannels) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    const auto executable = productionChildFixture();

    auto closed = makeProductionSessionSupervisorPlatform(
        SessionRuntimeOptions{executable, executable, executable},
        productionChildEnvironment("close"), runtime.value());
    ASSERT_TRUE(closed->launch(ComponentRole::shell, false));
    auto closedReady = closed->waitForShellReadiness(sessionShellReadinessAttemptTimeout);
    ASSERT_FALSE(closedReady);
    EXPECT_EQ(closedReady.error().code, ErrorCode::not_found);
    EXPECT_EQ(waitForProductionChild(*closed, ComponentRole::shell),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));

    auto malformed = makeProductionSessionSupervisorPlatform(
        SessionRuntimeOptions{executable, executable, executable},
        productionChildEnvironment("wrong-message"), runtime.value());
    ASSERT_TRUE(malformed->launch(ComponentRole::shell, false));
    auto malformedReady = malformed->waitForShellReadiness(sessionShellReadinessAttemptTimeout);
    ASSERT_FALSE(malformedReady);
    EXPECT_EQ(malformedReady.error().code, ErrorCode::validation_error);
    EXPECT_EQ(waitForProductionChild(*malformed, ComponentRole::shell),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_TRUE(runtime.value().cleanup());
}

} // namespace
} // namespace prismdrake::session
