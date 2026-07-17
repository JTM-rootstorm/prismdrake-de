#include "SessionEnvironment.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;

[[nodiscard]] bool hasEntry(const PreparedSessionEnvironment &environment,
                            std::string_view expected) {
    return std::ranges::find(environment.entries, expected) != environment.entries.end();
}

TEST(SessionEnvironmentTest, ReplacesOnlyDesktopIdentityAndPreservesUnrelatedValues) {
    constexpr std::string_view sentinel = "PRIVATE_SENTINEL=do-not-render-this-value";
    const std::array inherited{
        std::string_view{"DISPLAY=:77"},
        std::string_view{"XDG_RUNTIME_DIR=/run/user/1000"},
        std::string_view{"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus"},
        std::string_view{"PATH=/usr/bin:/bin"},
        std::string_view{"LANG=en_US.UTF-8"},
        std::string_view{"HOME=/home/tester"},
        sentinel,
        std::string_view{"XDG_CURRENT_DESKTOP=SomethingElse"},
        std::string_view{"XDG_SESSION_DESKTOP=other"},
        std::string_view{"DESKTOP_SESSION=other"},
    };

    const auto prepared = prepareSessionEnvironment(inherited);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared.value().display, ":77");
    EXPECT_EQ(prepared.value().runtimeDirectory, "/run/user/1000");
    EXPECT_EQ(prepared.value().sessionBusAddress, "unix:path=/run/user/1000/bus");
    EXPECT_TRUE(hasEntry(prepared.value(), "XDG_CURRENT_DESKTOP=Prismdrake"));
    EXPECT_TRUE(hasEntry(prepared.value(), "XDG_SESSION_DESKTOP=prismdrake"));
    EXPECT_TRUE(hasEntry(prepared.value(), "DESKTOP_SESSION=prismdrake"));
    EXPECT_TRUE(hasEntry(prepared.value(), "PATH=/usr/bin:/bin"));
    EXPECT_TRUE(hasEntry(prepared.value(), "LANG=en_US.UTF-8"));
    EXPECT_TRUE(hasEntry(prepared.value(), sentinel));
    EXPECT_FALSE(hasEntry(prepared.value(), "XDG_CURRENT_DESKTOP=SomethingElse"));
}

TEST(SessionEnvironmentTest, RejectsMissingEmptyRelativeAndDuplicateRequiredValues) {
    const std::array missingBus{
        std::string_view{"DISPLAY=:1"},
        std::string_view{"XDG_RUNTIME_DIR=/run/user/1000"},
    };
    const auto missing = prepareSessionEnvironment(missingBus);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::invalid_environment);

    const std::array emptyDisplay{
        std::string_view{"DISPLAY="},
        std::string_view{"XDG_RUNTIME_DIR=/run/user/1000"},
        std::string_view{"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
    };
    EXPECT_FALSE(prepareSessionEnvironment(emptyDisplay));

    const std::array relativeRuntime{
        std::string_view{"DISPLAY=:1"},
        std::string_view{"XDG_RUNTIME_DIR=relative"},
        std::string_view{"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
    };
    EXPECT_FALSE(prepareSessionEnvironment(relativeRuntime));

    const std::array duplicateDisplay{
        std::string_view{"DISPLAY=:1"},
        std::string_view{"DISPLAY=:2"},
        std::string_view{"XDG_RUNTIME_DIR=/run/user/1000"},
        std::string_view{"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
    };
    EXPECT_FALSE(prepareSessionEnvironment(duplicateDisplay));
}

TEST(SessionEnvironmentTest, RejectsMalformedAndOversizedInputWithoutReflectingValues) {
    const std::array malformed{
        std::string_view{"DISPLAY=:1"},
        std::string_view{"XDG_RUNTIME_DIR=/run/user/1000"},
        std::string_view{"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus"},
        std::string_view{"malformed-private-sentinel"},
    };
    const auto invalid = prepareSessionEnvironment(malformed);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().message.find("private-sentinel"), std::string::npos);
    EXPECT_EQ(invalid.error().recovery.find("private-sentinel"), std::string::npos);

    const std::string embeddedNull{"PRIVATE=value\0suffix", 20U};
    const std::array<std::string_view, 4U> truncatedEnvironment{
        "DISPLAY=:1", "XDG_RUNTIME_DIR=/run/user/1000",
        "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus", embeddedNull};
    EXPECT_FALSE(prepareSessionEnvironment(truncatedEnvironment));

    std::string oversized(maximumSessionEnvironmentEntryBytes + 1U, 'x');
    oversized.insert(0U, "PRIVATE=");
    const std::array<std::string_view, 4U> oversizedEnvironment{
        "DISPLAY=:1", "XDG_RUNTIME_DIR=/run/user/1000",
        "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/bus", oversized};
    EXPECT_FALSE(prepareSessionEnvironment(oversizedEnvironment));
}

} // namespace
} // namespace prismdrake::session
