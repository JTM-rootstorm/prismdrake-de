#include "NotificationReadiness.hpp"
#include "SessionSupervisor.hpp"

#include "SdBusApi.hpp"
#include "XdgPaths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

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

} // namespace
} // namespace prismdrake::session
