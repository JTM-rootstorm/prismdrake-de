#include "ShellControlService.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace prismdrake::shell::control;

class PresentationPort final : public ShellPresentationPort {
  public:
    // Test-only deadline-aware blocking probe; production ports are non-waiting.
    [[nodiscard]] ShellRequestFailure
    request(ShellAction action, std::string_view outputName,
            prismdrake::foundation::MonotonicClock::TimePoint deadline) override {
        if (block_until_deadline) {
            std::this_thread::sleep_until(deadline);
            return ShellRequestFailure::timed_out;
        }
        requests.emplace_back(action, std::string{outputName});
        return response;
    }

    bool block_until_deadline = false;
    ShellRequestFailure response = ShellRequestFailure::none;
    std::vector<std::pair<ShellAction, std::string>> requests;
};

TEST(ShellControlTest, RoutesOnlyFixedActionsAndPrimaryOutputSentinel) {
    PresentationPort port;
    ShellControlService service{port};
    EXPECT_EQ(service.request(ShellAction::toggle_launcher, ""), ShellRequestFailure::none);
    EXPECT_EQ(service.request(ShellAction::open_quick_settings, "DP-1"), ShellRequestFailure::none);
    EXPECT_EQ(service.request(ShellAction::open_notification_center, "HDMI-A-1"),
              ShellRequestFailure::none);
    EXPECT_EQ(service.request(ShellAction::focus_panel, "DP-1"), ShellRequestFailure::none);
    ASSERT_EQ(port.requests.size(), 4U);
    EXPECT_EQ(port.requests[0], (std::pair{ShellAction::toggle_launcher, std::string{}}));
}

TEST(ShellControlTest, AcceptsBoundedUnicodeAndRejectsUnsafeOutputNames) {
    PresentationPort port;
    ShellControlService service{port};
    EXPECT_EQ(service.request(ShellAction::focus_panel, "pantalla-δ"), ShellRequestFailure::none);
    EXPECT_EQ(service.request(ShellAction::focus_panel, "line\nbreak"),
              ShellRequestFailure::invalid_output);
    EXPECT_EQ(service.request(ShellAction::focus_panel, std::string(256U, 'x')),
              ShellRequestFailure::invalid_output);
    const std::string invalidUtf8{"\xc0\xaf", 2};
    EXPECT_EQ(service.request(ShellAction::focus_panel, invalidUtf8),
              ShellRequestFailure::invalid_output);
    EXPECT_EQ(port.requests.size(), 1U);
}

TEST(ShellControlTest, PreservesTypedPortFailuresAndStopsClosed) {
    PresentationPort port;
    ShellControlService service{port};
    port.response = ShellRequestFailure::output_unavailable;
    EXPECT_EQ(service.request(ShellAction::toggle_launcher, "missing"),
              ShellRequestFailure::output_unavailable);
    service.beginServiceStop();
    EXPECT_EQ(service.request(ShellAction::focus_panel, ""), ShellRequestFailure::service_stopping);
    EXPECT_EQ(port.requests.size(), 1U);
}

TEST(ShellControlTest, PropagatesARealCommitDeadlineWithoutLatePresentation) {
    using namespace std::chrono_literals;

    PresentationPort port;
    port.block_until_deadline = true;
    auto clock = std::make_shared<prismdrake::foundation::SystemMonotonicClock>();
    ShellControlService service{port, clock, 40ms};

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(service.request(ShellAction::toggle_launcher, ""), ShellRequestFailure::timed_out);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 40ms);
    EXPECT_TRUE(port.requests.empty());
    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(port.requests.empty());
}

TEST(ShellControlTest, RejectsAnExpiredBudgetBeforeCallingThePresentationPort) {
    auto clock = std::make_shared<prismdrake::foundation::TestMonotonicClock>();
    PresentationPort port;
    ShellControlService service{port, clock,
                                prismdrake::foundation::MonotonicClock::Duration::zero()};

    EXPECT_EQ(service.request(ShellAction::focus_panel, ""), ShellRequestFailure::timed_out);
    EXPECT_TRUE(port.requests.empty());
}

} // namespace
