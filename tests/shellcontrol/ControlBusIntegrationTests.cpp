#include "MonotonicClock.hpp"
#include "SessionControlService.hpp"
#include "ShellControlService.hpp"
#include "ShellCtl.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using prismdrake::ipc::sdbus::Bus;
using prismdrake::ipc::sdbus::Message;
using prismdrake::session::ExactChildShutdownPort;
using prismdrake::session::SessionControlService;
using prismdrake::shell::control::ShellAction;
using prismdrake::shell::control::ShellControlService;
using prismdrake::shell::control::ShellPresentationPort;
using prismdrake::shell::control::ShellRequestFailure;

constexpr char shellService[] = "org.prismdrake.Shell1";
constexpr char shellPath[] = "/org/prismdrake/Shell1";
constexpr char shellInterface[] = "org.prismdrake.Shell1";
constexpr char sessionService[] = "org.prismdrake.Session1";
constexpr char sessionPath[] = "/org/prismdrake/Session1";
constexpr char sessionInterface[] = "org.prismdrake.Session1";

struct Error final {
    sd_bus_error value{};
    ~Error() { sd_bus_error_free(&value); }
};

class ShutdownPort final : public ExactChildShutdownPort {
  public:
    // Test-only deadline-aware blocking probe; production admission is non-waiting.
    [[nodiscard]] prismdrake::session::ExactChildShutdownStart
    beginExactChildShutdown(std::uint64_t transactionId,
                            prismdrake::foundation::MonotonicClock::TimePoint deadline) override {
        if (block_until_deadline_.load()) {
            std::this_thread::sleep_until(deadline);
            return prismdrake::session::ExactChildShutdownStart::timed_out;
        }
        transaction_id_.store(transactionId);
        return prismdrake::session::ExactChildShutdownStart::started;
    }
    void setBlockUntilDeadline(bool block) noexcept { block_until_deadline_.store(block); }
    [[nodiscard]] std::uint64_t transactionId() const noexcept { return transaction_id_.load(); }

  private:
    std::atomic_bool block_until_deadline_ = false;
    std::atomic_uint64_t transaction_id_ = 0U;
};

class PresentationPort final : public ShellPresentationPort {
  public:
    // Test-only deadline-aware blocking probe; production ports are non-waiting.
    [[nodiscard]] ShellRequestFailure
    request(ShellAction action, std::string_view outputName,
            prismdrake::foundation::MonotonicClock::TimePoint deadline) override {
        if (outputName == "missing") {
            return ShellRequestFailure::output_unavailable;
        }
        if (block_until_deadline_.load()) {
            std::this_thread::sleep_until(deadline);
            return ShellRequestFailure::timed_out;
        }
        static_cast<void>(action);
        committed_count_.fetch_add(1U);
        return ShellRequestFailure::none;
    }
    void setBlockUntilDeadline(bool block) noexcept { block_until_deadline_.store(block); }
    [[nodiscard]] std::size_t committedCount() const noexcept { return committed_count_.load(); }

  private:
    std::atomic_bool block_until_deadline_ = false;
    std::atomic_size_t committed_count_ = 0U;
};

class ControlBusFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_GE(sd_bus_open_user(server_bus_.put()), 0);
        ASSERT_GE(sd_bus_open_user(client_bus_.put()), 0);
        ASSERT_TRUE(shell_service_.attach(server_bus_.get()));
        ASSERT_TRUE(session_service_.attach(server_bus_.get()));
        ASSERT_GE(sd_bus_request_name(server_bus_.get(), shellService, 0), 0);
        ASSERT_GE(sd_bus_request_name(server_bus_.get(), sessionService, 0), 0);
        server_thread_ = std::thread([this] { dispatch(); });
    }

    void TearDown() override {
        stop_.store(true);
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    [[nodiscard]] int callShell(std::string_view method, std::string_view output, Error &error) {
        Message reply;
        const std::string methodString{method};
        const std::string outputString{output};
        return sd_bus_call_method(client_bus_.get(), shellService, shellPath, shellInterface,
                                  methodString.c_str(), &error.value, reply.put(), "s",
                                  outputString.c_str());
    }

    [[nodiscard]] std::uint64_t requestLogout(Error &error) {
        Message reply;
        if (sd_bus_call_method(client_bus_.get(), sessionService, sessionPath, sessionInterface,
                               "RequestLogout", &error.value, reply.put(), "") < 0) {
            return 0;
        }
        std::uint64_t transactionId = 0;
        EXPECT_GE(sd_bus_message_read(reply.get(), "t", &transactionId), 0);
        return transactionId;
    }

    [[nodiscard]] int cancelLogout(std::uint64_t transactionId, Error &error) {
        Message reply;
        return sd_bus_call_method(client_bus_.get(), sessionService, sessionPath, sessionInterface,
                                  "CancelLogout", &error.value, reply.put(), "t", transactionId);
    }

    [[nodiscard]] int confirmLogout(std::uint64_t transactionId, Error &error) {
        Message reply;
        return sd_bus_call_method(client_bus_.get(), sessionService, sessionPath, sessionInterface,
                                  "ConfirmLogout", &error.value, reply.put(), "t", transactionId);
    }

    [[nodiscard]] std::pair<std::uint64_t, std::string> getSessionState(Error &error) {
        Message reply;
        if (sd_bus_call_method(client_bus_.get(), sessionService, sessionPath, sessionInterface,
                               "GetSessionState", &error.value, reply.put(), "") < 0) {
            return {};
        }
        std::uint64_t transactionId = 0;
        const char *state = nullptr;
        EXPECT_GE(sd_bus_message_read(reply.get(), "ts", &transactionId, &state), 0);
        return {transactionId, state != nullptr ? std::string{state} : std::string{}};
    }

    void dispatch() {
        while (!stop_.load()) {
            int result = 0;
            do {
                result = sd_bus_process(server_bus_.get(), nullptr);
            } while (result > 0);
            if (result < 0) {
                return;
            }
            (void)sd_bus_wait(server_bus_.get(), 20'000U);
        }
    }

    std::shared_ptr<prismdrake::foundation::SystemMonotonicClock> clock_ =
        std::make_shared<prismdrake::foundation::SystemMonotonicClock>();
    ShutdownPort shutdown_port_;
    PresentationPort presentation_port_;
    ShellControlService shell_service_{presentation_port_, clock_, std::chrono::milliseconds{40}};
    SessionControlService session_service_{shutdown_port_, clock_, std::chrono::seconds{1},
                                           std::chrono::milliseconds{40}};
    Bus server_bus_;
    Bus client_bus_;
    std::atomic_bool stop_ = false;
    std::thread server_thread_;
};

TEST_F(ControlBusFixture, ExportsExactSameUserShellMethodsAndTypedOutputFailure) {
    Error toggleError;
    EXPECT_GE(callShell("ToggleLauncher", "", toggleError), 0);
    Error quickError;
    EXPECT_GE(callShell("OpenQuickSettings", "DP-1", quickError), 0);
    Error notificationError;
    EXPECT_GE(callShell("OpenNotificationCenter", "", notificationError), 0);
    Error panelError;
    EXPECT_GE(callShell("FocusPanel", "DP-1", panelError), 0);

    Error missingError;
    EXPECT_LT(callShell("FocusPanel", "missing", missingError), 0);
    ASSERT_NE(missingError.value.name, nullptr);
    EXPECT_STREQ(missingError.value.name, "org.prismdrake.Shell1.Error.OutputUnavailable");
}

TEST_F(ControlBusFixture, KeepsOneCancellableLogoutTransaction) {
    Error requestError;
    const auto transactionId = requestLogout(requestError);
    ASSERT_NE(transactionId, 0U);

    Error busyError;
    EXPECT_EQ(requestLogout(busyError), 0U);
    ASSERT_NE(busyError.value.name, nullptr);
    EXPECT_STREQ(busyError.value.name, "org.prismdrake.Session1.Error.Busy");

    Error staleError;
    EXPECT_LT(cancelLogout(transactionId + 1U, staleError), 0);
    ASSERT_NE(staleError.value.name, nullptr);
    EXPECT_STREQ(staleError.value.name, "org.prismdrake.Session1.Error.TransactionMismatch");

    Error cancelError;
    EXPECT_GE(cancelLogout(transactionId, cancelError), 0);
}

TEST_F(ControlBusFixture, PropagatesShellCommitDeadlineWithoutLatePresentation) {
    using namespace std::chrono_literals;

    presentation_port_.setBlockUntilDeadline(true);
    Error error;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_LT(callShell("ToggleLauncher", "", error), 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_NE(error.value.name, nullptr);
    EXPECT_STREQ(error.value.name, "org.prismdrake.Shell1.Error.TimedOut");
    EXPECT_GE(elapsed, 40ms);
    EXPECT_EQ(presentation_port_.committedCount(), 0U);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(presentation_port_.committedCount(), 0U);
}

TEST_F(ControlBusFixture, PropagatesSessionAdmissionDeadlineWithoutLateShutdown) {
    using namespace std::chrono_literals;

    Error requestError;
    const auto transactionId = requestLogout(requestError);
    ASSERT_NE(transactionId, 0U);

    shutdown_port_.setBlockUntilDeadline(true);
    Error confirmError;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_LT(confirmLogout(transactionId, confirmError), 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_NE(confirmError.value.name, nullptr);
    EXPECT_STREQ(confirmError.value.name, "org.prismdrake.Session1.Error.TimedOut");
    EXPECT_GE(elapsed, 40ms);
    EXPECT_EQ(shutdown_port_.transactionId(), 0U);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(shutdown_port_.transactionId(), 0U);

    Error stateError;
    EXPECT_EQ(getSessionState(stateError),
              (std::pair{transactionId, std::string{"confirmation_requested"}}));

    Error cancelError;
    EXPECT_GE(cancelLogout(transactionId, cancelError), 0);
    Error idleStateError;
    EXPECT_EQ(getSessionState(idleStateError), (std::pair{std::uint64_t{0U}, std::string{"idle"}}));
}

TEST_F(ControlBusFixture, ShellctlMapsEveryFrozenCommandToTheTypedOwners) {
    using prismdrake::shellctl::Command;
    using prismdrake::shellctl::ExitStatus;
    EXPECT_EQ(prismdrake::shellctl::invoke(Command::launcher_toggle), ExitStatus::success);
    EXPECT_EQ(prismdrake::shellctl::invoke(Command::quick_settings_open), ExitStatus::success);
    EXPECT_EQ(prismdrake::shellctl::invoke(Command::notifications_open), ExitStatus::success);
    EXPECT_EQ(prismdrake::shellctl::invoke(Command::panel_focus), ExitStatus::success);
    EXPECT_EQ(prismdrake::shellctl::invoke(Command::session_logout), ExitStatus::success);
}

} // namespace
