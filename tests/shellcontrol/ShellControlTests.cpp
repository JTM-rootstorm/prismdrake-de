#include "ShellControlService.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace prismdrake::shell::control;

class PresentationPort final : public ShellPresentationPort {
  public:
    [[nodiscard]] ShellRequestFailure request(ShellAction action,
                                              std::string_view outputName) override {
        requests.emplace_back(action, std::string{outputName});
        return response;
    }

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

} // namespace
