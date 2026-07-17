#include "SessionOptions.hpp"

#include "BuildInfo.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace prismdrake::session {
namespace {

const SessionPathDefaults executableDefaults{"/bin/true", "/bin/false"};

TEST(SessionOptionsTest, HelpAndVersionDoNotRequireRuntimeExecutables) {
    const std::array helpArguments{std::string_view{"--help"}};
    const auto help = parseSessionOptions(helpArguments);
    ASSERT_TRUE(help);
    EXPECT_EQ(help.value().action, SessionAction::show_help);
    EXPECT_FALSE(help.value().runtime);
    EXPECT_NE(sessionHelpText().find("--settingsd PATH"), std::string::npos);
    EXPECT_NE(sessionHelpText().find("--shell PATH"), std::string::npos);

    const std::array versionArguments{std::string_view{"--version"}};
    const auto version = parseSessionOptions(versionArguments);
    ASSERT_TRUE(version);
    EXPECT_EQ(version.value().action, SessionAction::show_version);
    EXPECT_FALSE(version.value().runtime);
    EXPECT_EQ(sessionVersionText(),
              "prismdrake-session " + std::string{foundation::productVersion()} + '\n');
}

TEST(SessionOptionsTest, UsesValidatedCompiledDefaultsWithoutPathLookup) {
    const auto options = parseSessionOptions({}, executableDefaults);

    ASSERT_TRUE(options);
    ASSERT_TRUE(options.value().runtime);
    EXPECT_EQ(options.value().runtime->settingsdExecutable,
              std::filesystem::path{"/bin/true"}.lexically_normal());
    EXPECT_EQ(options.value().runtime->shellExecutable,
              std::filesystem::path{"/bin/false"}.lexically_normal());
}

TEST(SessionOptionsTest, ExplicitExecutablesOverrideDefaultsIndependently) {
    const std::array settingsdArguments{std::string_view{"--settingsd"},
                                        std::string_view{"/usr/bin/true"}};
    const auto settingsd = parseSessionOptions(settingsdArguments, executableDefaults);
    ASSERT_TRUE(settingsd);
    ASSERT_TRUE(settingsd.value().runtime);
    EXPECT_EQ(settingsd.value().runtime->settingsdExecutable,
              std::filesystem::path{"/usr/bin/true"}.lexically_normal());
    EXPECT_EQ(settingsd.value().runtime->shellExecutable,
              std::filesystem::path{"/bin/false"}.lexically_normal());

    const std::array shellArguments{std::string_view{"--shell"},
                                    std::string_view{"/usr/bin/false"}};
    const auto shell = parseSessionOptions(shellArguments, executableDefaults);
    ASSERT_TRUE(shell);
    ASSERT_TRUE(shell.value().runtime);
    EXPECT_EQ(shell.value().runtime->settingsdExecutable,
              std::filesystem::path{"/bin/true"}.lexically_normal());
    EXPECT_EQ(shell.value().runtime->shellExecutable,
              std::filesystem::path{"/usr/bin/false"}.lexically_normal());
}

TEST(SessionOptionsTest, RequiresBothRuntimeExecutablesWhenDefaultsAreUnavailable) {
    EXPECT_FALSE(parseSessionOptions({}));

    const std::array onlySettingsd{std::string_view{"--settingsd"}, std::string_view{"/bin/true"}};
    EXPECT_FALSE(parseSessionOptions(onlySettingsd));

    const std::array onlyShell{std::string_view{"--shell"}, std::string_view{"/bin/false"}};
    EXPECT_FALSE(parseSessionOptions(onlyShell));
}

TEST(SessionOptionsTest, AcceptsEachRuntimeExecutableExactlyOnceInEitherOrder) {
    const std::array arguments{std::string_view{"--shell"}, std::string_view{"/bin/false"},
                               std::string_view{"--settingsd"}, std::string_view{"/bin/true"}};
    const auto options = parseSessionOptions(arguments);

    ASSERT_TRUE(options);
    ASSERT_TRUE(options.value().runtime);
    EXPECT_EQ(options.value().runtime->settingsdExecutable, "/bin/true");
    EXPECT_EQ(options.value().runtime->shellExecutable, "/bin/false");
}

TEST(SessionOptionsTest, RejectsDuplicateMissingUnknownAndPositionalArguments) {
    const std::array duplicateSettingsd{
        std::string_view{"--settingsd"}, std::string_view{"/bin/true"},
        std::string_view{"--settingsd"}, std::string_view{"/bin/true"}};
    EXPECT_FALSE(parseSessionOptions(duplicateSettingsd, executableDefaults));

    const std::array duplicateShell{std::string_view{"--shell"}, std::string_view{"/bin/false"},
                                    std::string_view{"--shell"}, std::string_view{"/bin/false"}};
    EXPECT_FALSE(parseSessionOptions(duplicateShell, executableDefaults));

    const std::array missingAtEnd{std::string_view{"--settingsd"}};
    EXPECT_FALSE(parseSessionOptions(missingAtEnd, executableDefaults));

    const std::array missingBeforeOption{std::string_view{"--settingsd"},
                                         std::string_view{"--shell"},
                                         std::string_view{"/bin/false"}};
    EXPECT_FALSE(parseSessionOptions(missingBeforeOption, executableDefaults));

    const std::array unknown{std::string_view{"--unknown"}};
    EXPECT_FALSE(parseSessionOptions(unknown, executableDefaults));

    const std::array positional{std::string_view{"/bin/true"}};
    EXPECT_FALSE(parseSessionOptions(positional, executableDefaults));
}

TEST(SessionOptionsTest, RejectsConflictingAndRepeatedDisplayActions) {
    const std::array conflicting{std::string_view{"--help"}, std::string_view{"--version"}};
    EXPECT_FALSE(parseSessionOptions(conflicting));

    const std::array repeatedAlias{std::string_view{"-h"}, std::string_view{"--help"}};
    EXPECT_FALSE(parseSessionOptions(repeatedAlias));

    const std::array helpAndRuntime{std::string_view{"--help"}, std::string_view{"--settingsd"},
                                    std::string_view{"/bin/true"}};
    EXPECT_FALSE(parseSessionOptions(helpAndRuntime, executableDefaults));

    const std::array runtimeAndVersion{std::string_view{"--shell"}, std::string_view{"/bin/false"},
                                       std::string_view{"--version"}};
    EXPECT_FALSE(parseSessionOptions(runtimeAndVersion, executableDefaults));
}

TEST(SessionOptionsTest, RejectsInvalidUnavailableAndNonExecutablePathsWithoutEchoingThem) {
    constexpr std::string_view relativeSentinel = "private/sentinel-settingsd";
    const std::array relative{std::string_view{"--settingsd"}, relativeSentinel};
    const auto relativeResult = parseSessionOptions(relative, executableDefaults);
    ASSERT_FALSE(relativeResult);
    EXPECT_EQ(relativeResult.error().message.find(relativeSentinel), std::string::npos);
    EXPECT_EQ(relativeResult.error().recovery.find(relativeSentinel), std::string::npos);

    constexpr std::string_view missingSentinel = "/private/sentinel-shell-does-not-exist";
    const std::array missing{std::string_view{"--shell"}, missingSentinel};
    const auto missingResult = parseSessionOptions(missing, executableDefaults);
    ASSERT_FALSE(missingResult);
    EXPECT_EQ(missingResult.error().message.find(missingSentinel), std::string::npos);
    EXPECT_EQ(missingResult.error().recovery.find(missingSentinel), std::string::npos);

    const std::array directory{std::string_view{"--shell"}, std::string_view{"/tmp"}};
    EXPECT_FALSE(parseSessionOptions(directory, executableDefaults));

    const std::array nonExecutable{std::string_view{"--shell"}, std::string_view{"/etc/passwd"}};
    EXPECT_FALSE(parseSessionOptions(nonExecutable, executableDefaults));

    const std::string embeddedNull{"/bin/true\0private", 17U};
    const std::array nullArguments{std::string_view{"--settingsd"}, std::string_view{embeddedNull}};
    EXPECT_FALSE(parseSessionOptions(nullArguments, executableDefaults));

    const std::string oversizedPath = "/" + std::string(maximumSessionExecutablePathBytes, 'a');
    const std::array oversizedArguments{std::string_view{"--settingsd"},
                                        std::string_view{oversizedPath}};
    EXPECT_FALSE(parseSessionOptions(oversizedArguments, executableDefaults));
}

TEST(SessionOptionsTest, RejectsInvalidCompiledDefaultsWithoutEchoingThem) {
    const SessionPathDefaults invalidDefaults{"/private/sentinel-settingsd-does-not-exist",
                                              "/bin/false"};
    const auto result = parseSessionOptions({}, invalidDefaults);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message.find("sentinel-settingsd"), std::string::npos);
    EXPECT_EQ(result.error().recovery.find("sentinel-settingsd"), std::string::npos);
    EXPECT_EQ(result.error().code, foundation::ErrorCode::invalid_environment);
}

} // namespace
} // namespace prismdrake::session
