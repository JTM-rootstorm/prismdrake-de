#include "NotificationReadiness.hpp"
#include "SessionSupervisor.hpp"

#include "SdBusApi.hpp"
#include "XdgPaths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include <unistd.h>

namespace prismdrake::session {
namespace {

using namespace std::chrono_literals;

class RuntimeFixture final {
  public:
    RuntimeFixture() {
        std::string pattern = "/tmp/prismdrake-session-composition.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create the session composition fixture."};
        }
        root_ = created;
        runtime_base_ = root_ / "runtime";
        prismdrake_ = runtime_base_ / "prismdrake";
        std::filesystem::create_directories(prismdrake_);
        std::filesystem::permissions(runtime_base_, std::filesystem::perms::owner_all,
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
    std::filesystem::path runtime_base_;
    std::filesystem::path prismdrake_;
};

class ShutdownPort final : public ExactChildShutdownPort {
  public:
    [[nodiscard]] bool beginExactChildShutdown(std::uint64_t transactionId) override {
        observed_transaction_ = transactionId;
        return transactionId != 0U;
    }

  private:
    std::uint64_t observed_transaction_{0U};
};

TEST(SessionCompositionRuntimeTest, NotificationReadinessRequiresBothNamesFromTheExactProcess) {
    ipc::sdbus::Bus bus;
    ASSERT_GE(sd_bus_open_user(bus.put()), 0);
    ASSERT_GE(sd_bus_request_name(bus.get(), "org.freedesktop.Notifications", 0), 0);
    ASSERT_GE(sd_bus_request_name(bus.get(), "org.prismdrake.NotificationCenter1", 0), 0);

    EXPECT_TRUE(probeNotificationReadiness(::getpid(), 100ms));
    const auto wrongOwner = probeNotificationReadiness(::getpid() + 1, 100ms);
    ASSERT_FALSE(wrongOwner);
    EXPECT_EQ(wrongOwner.error().code, foundation::ErrorCode::unsupported);
}

TEST(SessionCompositionRuntimeTest, ProductionPlatformOwnsOneSessionControlEpoch) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    const SessionRuntimeOptions options{"/bin/true", "/bin/true", "/bin/true"};
    const PreparedSessionEnvironment environment{};
    ShutdownPort firstPort;
    ShutdownPort conflictingPort;

    auto first = makeProductionSessionSupervisorPlatform(options, environment, runtime.value());
    auto conflicting =
        makeProductionSessionSupervisorPlatform(options, environment, runtime.value());

    ASSERT_TRUE(first->ownSessionControl(firstPort));
    const auto conflict = conflicting->ownSessionControl(conflictingPort);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, foundation::ErrorCode::invalid_environment);
    EXPECT_FALSE(first->dispatchSessionControl(0U));
    EXPECT_TRUE(first->dispatchSessionControl(maximumSessionControlMessagesPerTick));
    first->beginSessionControlStop();
    EXPECT_TRUE(runtime.value().cleanup());
}

TEST(SessionCompositionRuntimeTest, ProductionActivityWaitWakesForSessionBusWork) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(installSessionSignalHandlers());
    const SessionRuntimeOptions options{"/bin/true", "/bin/true", "/bin/true"};
    const PreparedSessionEnvironment environment{
        {"DISPLAY=:77", "XDG_RUNTIME_DIR=/tmp", "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
        ":77",
        "/tmp",
        "unix:path=/tmp/bus"};
    ShutdownPort shutdownPort;
    auto platform =
        makeProductionSessionSupervisorPlatform(options, environment, runtime.value());
    ASSERT_TRUE(platform->ownSessionControl(shutdownPort));

    ipc::sdbus::Bus client;
    ASSERT_GE(sd_bus_open_user(client.put()), 0);
    ipc::sdbus::Message request;
    ASSERT_GE(sd_bus_message_new_method_call(
                  client.get(), request.put(), "org.prismdrake.Session1",
                  "/org/prismdrake/Session1", "org.prismdrake.Session1", "GetSessionState"),
              0);
    ASSERT_GE(sd_bus_send(client.get(), request.get(), nullptr), 0);
    ASSERT_GE(sd_bus_flush(client.get()), 0);

    const auto activity = platform->waitForActivity();
    const auto dispatched = platform->dispatchSessionControl(maximumSessionControlMessagesPerTick);

    EXPECT_TRUE(activity);
    EXPECT_TRUE(dispatched);
    platform->beginSessionControlStop();
    EXPECT_TRUE(runtime.value().cleanup());
}

TEST(SessionCompositionRuntimeTest, ProductionActivityWaitRetainsChildExitDeliveredBeforeWait) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(installSessionSignalHandlers());
    const SessionRuntimeOptions options{"/bin/true", "/bin/true", "/bin/true"};
    const PreparedSessionEnvironment environment{
        {"DISPLAY=:77", "XDG_RUNTIME_DIR=/tmp", "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
        ":77",
        "/tmp",
        "unix:path=/tmp/bus"};
    ShutdownPort shutdownPort;
    auto platform =
        makeProductionSessionSupervisorPlatform(options, environment, runtime.value());
    ASSERT_TRUE(platform->ownSessionControl(shutdownPort));
    ASSERT_TRUE(platform->launch(ComponentRole::settingsd, false));

    std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(platform->waitForActivity());
    const auto status = platform->poll(ComponentRole::settingsd);

    ASSERT_TRUE(status);
    ASSERT_TRUE(status.value());
    EXPECT_EQ(status.value()->kind, ChildExitKind::exited);
    EXPECT_EQ(status.value()->value, 0);
    platform->beginSessionControlStop();
    EXPECT_TRUE(runtime.value().cleanup());
}

TEST(SessionCompositionRuntimeTest, ProductionActivityWaitWakesForShutdownSignal) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(installSessionSignalHandlers());
    const SessionRuntimeOptions options{"/bin/true", "/bin/true", "/bin/true"};
    ShutdownPort shutdownPort;
    auto platform = makeProductionSessionSupervisorPlatform(options, {}, runtime.value());
    ASSERT_TRUE(platform->ownSessionControl(shutdownPort));

    std::thread signaler([] {
        std::this_thread::sleep_for(10ms);
        static_cast<void>(::kill(::getpid(), SIGTERM));
    });
    const auto activity = platform->waitForActivity();
    signaler.join();

    EXPECT_TRUE(activity);
    EXPECT_TRUE(platform->shutdownRequested());
    platform->beginSessionControlStop();
    EXPECT_TRUE(runtime.value().cleanup());
}

} // namespace
} // namespace prismdrake::session
