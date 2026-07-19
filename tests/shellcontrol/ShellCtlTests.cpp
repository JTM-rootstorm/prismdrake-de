#include "ShellCtl.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

using namespace prismdrake::shellctl;

TEST(ShellCtlTest, ParsesOnlyTheFiveFrozenCommands) {
    struct Case final {
        std::array<std::string_view, 2> arguments;
        Command command;
    };
    const std::array cases{
        Case{{"launcher", "toggle"}, Command::launcher_toggle},
        Case{{"quick-settings", "open"}, Command::quick_settings_open},
        Case{{"notifications", "open"}, Command::notifications_open},
        Case{{"panel", "focus"}, Command::panel_focus},
        Case{{"session", "logout"}, Command::session_logout},
    };
    for (const auto &testCase : cases) {
        const auto parsed = parseCommand(testCase.arguments);
        EXPECT_TRUE(parsed.valid());
        EXPECT_EQ(parsed.command, testCase.command);
    }
}

TEST(ShellCtlTest, RejectsArbitraryNamesArgumentsAndShellText) {
    const std::array<std::string_view, 0> empty{};
    const std::array one{std::string_view{"launcher"}};
    const std::array wrongAction{std::string_view{"launcher"}, std::string_view{"open"}};
    const std::array extra{std::string_view{"panel"}, std::string_view{"focus"},
                           std::string_view{"DP-1"}};
    const std::array shellText{std::string_view{"session"}, std::string_view{"logout; reboot"}};
    EXPECT_FALSE(parseCommand(empty).valid());
    EXPECT_FALSE(parseCommand(one).valid());
    EXPECT_FALSE(parseCommand(wrongAction).valid());
    EXPECT_FALSE(parseCommand(extra).valid());
    EXPECT_FALSE(parseCommand(shellText).valid());
}

} // namespace
