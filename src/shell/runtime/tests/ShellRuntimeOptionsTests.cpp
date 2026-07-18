#include "ShellRuntimeOptions.hpp"

#include "ProcessLaunch.hpp"
#include "SessionReadinessProtocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

TEST(ShellRuntimeOptionsTest, PublishesExactReadinessMessageAndStripsPrivateDescriptor) {
    const int descriptor = ::eventfd(0U, EFD_NONBLOCK);
    ASSERT_GE(descriptor, 0);
    const int observer = ::dup(descriptor);
    ASSERT_GE(observer, 0);
    const std::string privateEntry =
        std::string{foundation::sessionReadinessDescriptorEnvironment} + '=' +
        std::to_string(descriptor);
    const std::vector<std::string_view> inherited{"PUBLIC=value", privateEntry};

    auto signal = sessionReadinessSignalFromEnvironment(inherited);
    ASSERT_TRUE(signal) << signal.error().message;
    EXPECT_TRUE(signal.value().pending());
    EXPECT_NE(::fcntl(std::stoi(privateEntry.substr(privateEntry.find('=') + 1U)), F_GETFD) &
                  FD_CLOEXEC,
              0);
    auto publicEnvironment = validatedLaunchEnvironment(inherited);
    ASSERT_TRUE(publicEnvironment);
    EXPECT_EQ(publicEnvironment.value(), (std::vector<std::string>{"PUBLIC=value"}));

    ASSERT_TRUE(signal.value().publish());
    EXPECT_FALSE(signal.value().pending());
    std::uint64_t message = 0U;
    EXPECT_EQ(::read(observer, &message, sizeof(message)), static_cast<ssize_t>(sizeof(message)));
    EXPECT_EQ(message, foundation::sessionReadinessMessage);
    static_cast<void>(::close(observer));
}

TEST(ShellRuntimeOptionsTest, RejectsDuplicateMalformedAndForeignReadinessDescriptorsRedacted) {
    const auto name = foundation::sessionReadinessDescriptorEnvironment;
    const std::string malformed = std::string{name} + "=secret-not-a-descriptor";
    auto invalid = sessionReadinessSignalFromEnvironment(std::vector<std::string_view>{malformed});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, foundation::ErrorCode::invalid_environment);
    EXPECT_EQ(invalid.error().message.find("secret"), std::string::npos);

    std::array<int, 2U> pipeDescriptors{-1, -1};
    ASSERT_EQ(::pipe(pipeDescriptors.data()), 0);
    const std::string foreign = std::string{name} + '=' + std::to_string(pipeDescriptors[1]);
    auto wrongType = sessionReadinessSignalFromEnvironment(std::vector<std::string_view>{foreign});
    ASSERT_FALSE(wrongType);
    EXPECT_EQ(wrongType.error().code, foundation::ErrorCode::invalid_environment);
    static_cast<void>(::close(pipeDescriptors[0]));
    static_cast<void>(::close(pipeDescriptors[1]));

    const std::string descriptor = std::string{name} + "=3";
    auto duplicate = sessionReadinessSignalFromEnvironment(
        std::vector<std::string_view>{descriptor, descriptor});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, foundation::ErrorCode::invalid_environment);
}

} // namespace
} // namespace prismdrake::shell::runtime
