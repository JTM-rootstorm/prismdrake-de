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

[[nodiscard]] PreparedSessionEnvironment productionChildEnvironment() {
    return PreparedSessionEnvironment{
        {"DISPLAY=:77", "XDG_RUNTIME_DIR=/tmp", "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus",
         "SUPERVISOR_FIXTURE=exact-value", "XDG_CURRENT_DESKTOP=Prismdrake",
         "XDG_SESSION_DESKTOP=prismdrake", "DESKTOP_SESSION=prismdrake"},
        ":77",
        "/tmp",
        "unix:path=/tmp/bus"};
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
        auto &failures =
            component == ComponentRole::settingsd ? settingsLaunchFailures : shellLaunchFailures;
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
        auto &exits = component == ComponentRole::settingsd ? settingsExits : shellExits;
        const bool exitEligible = component != ComponentRole::settingsd ||
                                  !settingsExitsAfterReadyOnly || readyMarks > 0U;
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
        return component == ComponentRole::settingsd ? settingsRunning : shellRunning;
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

    Result<void> markReady() override {
        ++readyMarks;
        if (shutdownOnReady) {
            requestShutdown = true;
        }
        return Result<void>::success();
    }

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
    std::size_t shellLaunchFailures{0U};
    std::size_t settingsReadinessFailures{0U};
    std::size_t readinessUnavailableThroughSettingsLaunch{0U};
    std::size_t pollFailures{0U};
    ErrorCode settingsReadinessFailureCode{ErrorCode::not_found};
    bool pollFailuresAfterReadyOnly{false};
    bool settingsExitsAfterReadyOnly{false};
    bool settingsRunning{false};
    bool shellRunning{false};
    bool settingsIgnoreTerminate{false};
    bool shellIgnoreTerminate{false};
    bool settingsTerminateRequested{false};
    bool shellTerminateRequested{false};
    bool settingsKillRequested{false};
    bool shellKillRequested{false};
    bool requestShutdown{false};
    bool shutdownOnReady{false};
    std::optional<std::size_t> shutdownAfterLaunches;
    std::optional<std::size_t> shutdownAfterReadinessCalls;
    std::optional<std::size_t> shutdownAfterWaits;
    std::deque<ChildExitStatus> settingsExits;
    std::deque<ChildExitStatus> shellExits;
    MonotonicClock::TimePoint currentTime{};
    std::vector<LaunchRecord> launches;
    std::vector<ComponentRole> polls;
    std::vector<ComponentRole> stopOrder;
    std::vector<ComponentRole> forced;
    std::vector<std::chrono::milliseconds> waits;
    std::vector<std::chrono::milliseconds> readinessTimeouts;
    std::vector<DiagnosticEventId> diagnostics;
    std::size_t readyMarks{0U};
    std::size_t safeModeMarks{0U};

  private:
    [[nodiscard]] bool &runningState(ComponentRole component) noexcept {
        return component == ComponentRole::settingsd ? settingsRunning : shellRunning;
    }
    [[nodiscard]] bool &terminateRequested(ComponentRole component) noexcept {
        return component == ComponentRole::settingsd ? settingsTerminateRequested
                                                     : shellTerminateRequested;
    }
    [[nodiscard]] bool &killRequested(ComponentRole component) noexcept {
        return component == ComponentRole::settingsd ? settingsKillRequested : shellKillRequested;
    }
    [[nodiscard]] bool ignoreTerminate(ComponentRole component) const noexcept {
        return component == ComponentRole::settingsd ? settingsIgnoreTerminate
                                                     : shellIgnoreTerminate;
    }
};

[[nodiscard]] SessionSupervisor
supervisor(FakePlatform &platform, SessionRecoveryConfig config = pd1SessionRecoveryConfig()) {
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
    ASSERT_EQ(platform.launches.size(), 2U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::shell);
    EXPECT_FALSE(platform.launches[0].safeMode);
    EXPECT_EQ(platform.readinessTimeouts,
              (std::vector<std::chrono::milliseconds>{sessionSettingsReadinessAttemptTimeout}));
    EXPECT_EQ(platform.readyMarks, 1U);
    EXPECT_EQ(platform.stopOrder, (std::vector<ComponentRole>{ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, RestartsCrashedShellWithBoundedBackoffWithoutStoppingSettings) {
    FakePlatform platform;
    platform.shellExits.push_back({ChildExitKind::exited, 17, false});
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
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
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::shell);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::settingsd);
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
    EXPECT_NE(safeSettings, platform.launches.end());
    EXPECT_NE(safeShell, platform.launches.end());
}

TEST(SessionSupervisorTest, TransientReadinessFailureRetriesWithoutRestartingSettings) {
    FakePlatform platform;
    platform.settingsReadinessFailures = 1U;
    platform.shellExits.push_back({ChildExitKind::exited, 0, false});
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::clean);
    ASSERT_EQ(platform.launches.size(), 2U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::shell);
    EXPECT_EQ(platform.readinessTimeouts.size(), 2U);
    EXPECT_EQ(platform.readinessTimeouts[0], sessionSettingsReadinessAttemptTimeout);
    EXPECT_EQ(platform.waits.front(), sessionMonitorInterval);
    EXPECT_EQ(std::accumulate(platform.waits.begin(), platform.waits.end(), 0ms),
              sessionMonitorInterval);
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
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
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
    EXPECT_EQ(platform.waits, (std::vector<std::chrono::milliseconds>{sessionMonitorInterval}));
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
    ASSERT_EQ(platform.launches.size(), 3U);
    EXPECT_EQ(platform.launches[0].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[1].component, ComponentRole::settingsd);
    EXPECT_EQ(platform.launches[2].component, ComponentRole::shell);
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
    EXPECT_EQ(platform.launches.size(), 2U);
    EXPECT_EQ(platform.stopOrder, (std::vector<ComponentRole>{ComponentRole::settingsd}));
}

TEST(SessionSupervisorTest, UnresponsiveChildrenAreKilledAndReportedInReverseOrder) {
    FakePlatform platform;
    platform.settingsIgnoreTerminate = true;
    platform.shellIgnoreTerminate = true;
    platform.shutdownOnReady = true;
    auto controller = supervisor(platform);

    const auto outcome = controller.run();

    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome.value(), SessionTermination::shutdown_requested);
    EXPECT_EQ(platform.stopOrder,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::settingsd}));
    EXPECT_EQ(platform.forced,
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::settingsd}));
    EXPECT_GE(std::count(platform.diagnostics.begin(), platform.diagnostics.end(),
                         DiagnosticEventId::operation_cancelled),
              3);
    EXPECT_LE(platform.currentTime.time_since_epoch(),
              2U * (sessionTerminateGrace + sessionKillReapGrace) + sessionMonitorInterval);
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
              (std::vector<ComponentRole>{ComponentRole::shell, ComponentRole::settingsd}));
    EXPECT_FALSE(platform.shellRunning);
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
    auto platform =
        makeProductionSessionSupervisorPlatform(SessionRuntimeOptions{executable, executable},
                                                productionChildEnvironment(), runtime.value());

    ASSERT_TRUE(platform->launch(ComponentRole::shell, false));
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::shell),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::shell));

    ASSERT_TRUE(platform->launch(ComponentRole::settingsd, true));
    EXPECT_EQ(waitForProductionChild(*platform, ComponentRole::settingsd),
              (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(platform->running(ComponentRole::settingsd));
    ASSERT_TRUE(platform->markSafeMode());
    ASSERT_TRUE(platform->markReady());
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().safeModeMarkerPath()));
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().readyMarkerPath()));
    EXPECT_TRUE(runtime.value().cleanup());
}

} // namespace
} // namespace prismdrake::session
