#include "SessionControlService.hpp"
#include "ShellControlService.hpp"
#include "ShellCtl.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    [[nodiscard]] bool beginExactChildShutdown(std::uint64_t transactionId) override {
        transactionId_ = transactionId;
        return true;
    }
    std::uint64_t transactionId_ = 0;
};

class PresentationPort final : public ShellPresentationPort {
  public:
    [[nodiscard]] ShellRequestFailure request(ShellAction action,
                                              std::string_view outputName) override {
        if (outputName == "missing") {
            return ShellRequestFailure::output_unavailable;
        }
        requests.emplace_back(action, std::string{outputName});
        return ShellRequestFailure::none;
    }
    std::vector<std::pair<ShellAction, std::string>> requests;
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

    ShutdownPort shutdown_port_;
    PresentationPort presentation_port_;
    ShellControlService shell_service_{presentation_port_};
    SessionControlService session_service_{shutdown_port_};
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
