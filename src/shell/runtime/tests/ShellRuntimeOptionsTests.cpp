#include "ShellRuntimeOptions.hpp"

#include "ProcessLaunch.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

TEST(ShellRuntimeOptionsTest, PreservesInheritedEnvironmentBytesAndOrder) {
    const std::string nonUtf8{"RAW=\xff", 5U};
    const std::vector<std::string_view> inherited{"BETA=two words", nonUtf8, "ALPHA=1"};

    const auto environment = validatedLaunchEnvironment(inherited);

    ASSERT_TRUE(environment);
    EXPECT_EQ(environment.value(),
              (std::vector<std::string>{"BETA=two words", nonUtf8, "ALPHA=1"}));
}

TEST(ShellRuntimeOptionsTest, RejectsMalformedAndOversizedEntriesWithoutDisclosure) {
    const std::string embeddedNull{"SECRET=value\0trailing", 21U};
    for (const std::vector<std::string_view> &inherited :
         {std::vector<std::string_view>{""}, std::vector<std::string_view>{"NO_SEPARATOR"},
          std::vector<std::string_view>{"=missing-name"},
          std::vector<std::string_view>{embeddedNull}}) {
        const auto environment = validatedLaunchEnvironment(inherited);
        ASSERT_FALSE(environment);
        EXPECT_EQ(environment.error().code, foundation::ErrorCode::invalid_environment);
        EXPECT_EQ(environment.error().message.find("NO_SEPARATOR"), std::string::npos);
    }

    const std::string oversized =
        "KEY=" + std::string(prismdrake::launcher::maximumProcessLaunchEnvironmentEntryBytes, 'x');
    const auto environment = validatedLaunchEnvironment(std::vector<std::string_view>{oversized});
    ASSERT_FALSE(environment);
    EXPECT_EQ(environment.error().code, foundation::ErrorCode::too_large);
    EXPECT_EQ(environment.error().message.find(std::string(16U, 'x')), std::string::npos);
}

} // namespace
} // namespace prismdrake::shell::runtime
