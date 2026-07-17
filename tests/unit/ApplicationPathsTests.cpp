#include "ApplicationPaths.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

TEST(ApplicationPathsTest, UsesStandardDefaultsInXdgPrecedenceOrder) {
    ApplicationPathEnvironment environment;
    environment.home = "/home/tester";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/home/tester/.local/share/applications",
                                                  "/usr/local/share/applications",
                                                  "/usr/share/applications"}));
}

TEST(ApplicationPathsTest, EmptyXdgValuesUseStandardDefaults) {
    ApplicationPathEnvironment environment;
    environment.home = "/home/tester";
    environment.dataHome = "";
    environment.dataDirectories = "";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/home/tester/.local/share/applications",
                                                  "/usr/local/share/applications",
                                                  "/usr/share/applications"}));
}

TEST(ApplicationPathsTest, HonorsAbsoluteDataHomeWithoutRequiringValidHome) {
    ApplicationPathEnvironment environment;
    environment.home = "relative/private-home";
    environment.dataHome = "/vol/user-data";
    environment.dataDirectories = "/vol/system-data";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/vol/user-data/applications",
                                                  "/vol/system-data/applications"}));
}

TEST(ApplicationPathsTest, RelativeDataHomeFallsBackToAbsoluteHome) {
    ApplicationPathEnvironment environment;
    environment.home = "/home/tester";
    environment.dataHome = "relative/private-data";
    environment.dataDirectories = "/system/data";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories.front(),
              "/home/tester/.local/share/applications");
}

TEST(ApplicationPathsTest, RejectsMissingOrRelativeHomeOnlyWhenFallbackIsNeeded) {
    ApplicationPathEnvironment missingHome;
    missingHome.dataDirectories = "/system/data";
    const auto missing = resolveApplicationPaths(missingHome);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(missing.error().message,
              "HOME is required when the XDG data-home default is needed.");

    ApplicationPathEnvironment relativeHome;
    relativeHome.home = "relative/private-home";
    relativeHome.dataHome = "also-relative";
    relativeHome.dataDirectories = "/system/data";
    const auto relative = resolveApplicationPaths(relativeHome);
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(relative.error().message,
              "HOME must be absolute when the XDG data-home default is needed.");
}

TEST(ApplicationPathsTest, IgnoresEmptyAndRelativeDataDirectoryComponents) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/user/data";
    environment.dataDirectories = ":relative:/first::also-relative:/second:";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/user/data/applications", "/first/applications",
                                                  "/second/applications"}));
}

TEST(ApplicationPathsTest, NonemptyListWithNoValidSystemRootsKeepsUserRoot) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/user/data";
    environment.dataDirectories = ":relative::also-relative:";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/user/data/applications"}));
}

TEST(ApplicationPathsTest, DeduplicatesNormalizedDirectoriesWithoutChangingPrecedence) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/first/../shared";
    environment.dataDirectories = "/shared:/second/./:/shared/";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/shared/applications", "/second/applications"}));
}

TEST(ApplicationPathsTest, TreatsShellSyntaxAndWhitespaceAsLiteralPathContent) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/with space/$HOME/~";
    environment.dataDirectories = "~:$HOME:.:/system path/$HOME";

    const auto result = resolveApplicationPaths(environment);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().applicationDirectories,
              (std::vector<std::filesystem::path>{"/with space/$HOME/~/applications",
                                                  "/system path/$HOME/applications"}));
}

TEST(ApplicationPathsTest, AcceptsExactComponentLimitBeforeRejectingOneMore) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/user/data";
    environment.dataDirectories = "";
    for (std::size_t index = 0U; index < maximumApplicationDataDirectoryEntries; ++index) {
        if (!environment.dataDirectories->empty()) {
            environment.dataDirectories->push_back(':');
        }
        environment.dataDirectories->append("/root-");
        environment.dataDirectories->append(std::to_string(index));
    }

    const auto exact = resolveApplicationPaths(environment);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().applicationDirectories.size(),
              maximumApplicationDataDirectoryEntries + 1U);

    environment.dataDirectories->append(":/overflow");
    const auto oneMore = resolveApplicationPaths(environment);
    ASSERT_FALSE(oneMore);
    EXPECT_EQ(oneMore.error().code, ErrorCode::too_large);
}

TEST(ApplicationPathsTest, BoundsTheCompleteEnvironmentEnvelopeEvenForUnusedValues) {
    ApplicationPathEnvironment exact;
    exact.dataHome = std::string(maximumApplicationPathValueBytes, 'h');
    exact.dataHome->front() = '/';
    exact.dataDirectories = std::string(maximumApplicationPathValueBytes, 'd');
    exact.dataDirectories->front() = '/';

    const auto accepted = resolveApplicationPaths(exact);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().applicationDirectories.size(), 2U);

    exact.home = "x";
    const auto aggregateOverflow = resolveApplicationPaths(exact);
    ASSERT_FALSE(aggregateOverflow);
    EXPECT_EQ(aggregateOverflow.error().code, ErrorCode::too_large);

    ApplicationPathEnvironment unusedNull;
    unusedNull.home = std::string{"/unused\0private-sentinel", 24U};
    unusedNull.dataHome = "/user/data";
    const auto invalid = resolveApplicationPaths(unusedNull);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(invalid.error().message.find("private-sentinel"), std::string::npos);
    EXPECT_EQ(invalid.error().recovery.find("private-sentinel"), std::string::npos);
}

TEST(ApplicationPathsTest, RejectsTooManyComponentsWithStaticRedactedError) {
    ApplicationPathEnvironment environment;
    environment.dataHome = "/user/data";
    environment.dataDirectories = "";
    for (std::size_t index = 0U; index <= maximumApplicationDataDirectoryEntries; ++index) {
        if (!environment.dataDirectories->empty()) {
            environment.dataDirectories->push_back(':');
        }
        environment.dataDirectories->append("private-relative-entry");
    }

    const auto result = resolveApplicationPaths(environment);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
    EXPECT_EQ(result.error().message, "XDG_DATA_DIRS contains too many directory entries.");
    EXPECT_EQ(result.error().message.find("private-relative-entry"), std::string::npos);
    EXPECT_EQ(result.error().recovery.find("private-relative-entry"), std::string::npos);
}

TEST(ApplicationPathsTest, RejectsOversizedOrEmbeddedNullInputWithoutDisclosure) {
    ApplicationPathEnvironment oversized;
    oversized.dataHome = "/user/data";
    oversized.dataDirectories = std::string(maximumApplicationPathValueBytes + 1U, 'x');
    const auto tooLarge = resolveApplicationPaths(oversized);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, ErrorCode::too_large);

    constexpr std::string_view privateValue = "private-sentinel";
    ApplicationPathEnvironment embeddedNull;
    embeddedNull.dataHome = "/user/data";
    embeddedNull.dataDirectories = std::string{"/first\0private-sentinel", 23U};
    const auto invalid = resolveApplicationPaths(embeddedNull);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(invalid.error().message.find(privateValue), std::string::npos);
    EXPECT_EQ(invalid.error().recovery.find(privateValue), std::string::npos);
}

} // namespace
} // namespace prismdrake::launcher
