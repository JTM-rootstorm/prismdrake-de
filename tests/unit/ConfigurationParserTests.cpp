#include "ConfigurationParser.hpp"

#include "BuildInfo.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::config {
namespace {

using foundation::ErrorCode;

[[nodiscard]] std::string readFixture(std::string_view relativePath) {
    const std::filesystem::path path = std::filesystem::path{PRISMDRAKE_SOURCE_DIR} / relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open committed configuration fixture");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string validConfiguration() {
    return readFixture("examples/config/lustre.toml");
}

[[nodiscard]] std::string replaceOnce(std::string input, std::string_view before,
                                      std::string_view after) {
    const auto position = input.find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("configuration test replacement target is absent");
    }
    input.replace(position, before.size(), after);
    return input;
}

[[nodiscard]] std::string outputArray(std::size_t count) {
    std::ostringstream output;
    output << "outputs = [";
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << "\"output-" << index << "\"";
    }
    output << ']';
    return output.str();
}

TEST(ConfigurationParserTest, ParsesEveryCommittedCompleteConfiguration) {
    constexpr std::array fixtures{
        std::string_view{"examples/config/lustre.toml"},
        std::string_view{"examples/config/forge.toml"},
        std::string_view{"examples/config/accessible.toml"},
        std::string_view{"data/defaults/config.toml"},
    };

    for (const auto fixture : fixtures) {
        const auto result = parseConfigurationToml(readFixture(fixture));
        ASSERT_TRUE(result) << fixture << ": " << result.error().message;
        EXPECT_EQ(result.value().schemaVersion, 1U);
    }
}

TEST(ConfigurationParserTest, NormalizesEveryDomainIntoTypedValues) {
    const auto result = parseConfigurationToml(validConfiguration());

    ASSERT_TRUE(result);
    const auto &config = result.value();
    EXPECT_EQ(config.profile, Profile::lustre);
    EXPECT_EQ(config.appearance.accent, "#4D7FFF");
    EXPECT_TRUE(config.appearance.transparencyEnabled);
    EXPECT_EQ(config.appearance.blurQuality, BlurQuality::balanced);
    EXPECT_DOUBLE_EQ(config.appearance.textScale, 1.0);
    EXPECT_EQ(config.panel.edge, PanelEdge::bottom);
    EXPECT_EQ(config.panel.sizePx, 48U);
    EXPECT_EQ(config.panel.grouping, GroupingMode::when_full);
    ASSERT_EQ(config.launcher.searchProviders.size(), 2U);
    EXPECT_EQ(config.notifications.defaultTimeoutMs, 5000U);
    EXPECT_EQ(config.desktop.wallpaperMode, WallpaperMode::fill);
    EXPECT_TRUE(config.integration.exportXsettings);
    EXPECT_EQ(config.accessibility.minimumTargetSizePx, 40U);
    EXPECT_TRUE(config.keyboard.menuKeyOpensLauncher);
    EXPECT_FALSE(config.developer.diagnosticsEnabled);
    EXPECT_TRUE(config.developer.mockCapabilityOverrides.empty());
}

TEST(ConfigurationParserTest, RebuildsOnlyTheRequestedProfile) {
    const auto parsed = parseConfigurationToml(validConfiguration());
    ASSERT_TRUE(parsed);

    const auto forge = withProfile(parsed.value(), Profile::forge);
    EXPECT_EQ(forge.profile, Profile::forge);
    EXPECT_EQ(forge.appearance, parsed.value().appearance);
    EXPECT_EQ(forge.panel, parsed.value().panel);
    EXPECT_EQ(forge.accessibility, parsed.value().accessibility);
    EXPECT_EQ(forge.developer, parsed.value().developer);
}

TEST(ConfigurationParserTest, KeepsSyntaxAndSemanticValidationSeparate) {
    const auto incomplete = replaceOnce(validConfiguration(), "schema_version = 1\n", "");

    auto parsed = parseConfigurationSyntax(incomplete);
    ASSERT_TRUE(parsed);
    const auto validated = validateConfiguration(parsed.value());

    ASSERT_FALSE(validated);
    EXPECT_EQ(validated.error().code, ErrorCode::validation_error);
    EXPECT_NE(validated.error().message.find("$.schema_version"), std::string::npos);
}

TEST(ConfigurationParserTest, RejectsEmptyMalformedDuplicateAndNonUtf8Input) {
    const auto empty = parseConfigurationToml("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::syntax_error);

    const auto malformed = parseConfigurationToml("schema_version = \"");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, ErrorCode::syntax_error);

    const auto duplicateInput = replaceOnce(validConfiguration(), "profile = \"lustre\"",
                                            "profile = \"lustre\"\nprofile = \"forge\"");
    const auto duplicate = parseConfigurationToml(duplicateInput);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::syntax_error);

    std::string nonUtf8 = validConfiguration() + "\n#";
    nonUtf8.push_back(static_cast<char>(0xff));
    const auto invalidEncoding = parseConfigurationToml(nonUtf8);
    ASSERT_FALSE(invalidEncoding);
    EXPECT_EQ(invalidEncoding.error().code, ErrorCode::syntax_error);
}

TEST(ConfigurationParserTest, EnforcesTheBoundedInMemoryEntryPoint) {
    std::string exact = validConfiguration() + "\n#";
    ASSERT_LT(exact.size(), maximumConfigurationBytes);
    exact.append(maximumConfigurationBytes - exact.size(), 'x');
    ASSERT_EQ(exact.size(), maximumConfigurationBytes);
    EXPECT_TRUE(parseConfigurationToml(exact));

    exact.push_back('x');
    const auto oversized = parseConfigurationToml(exact);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(ConfigurationParserTest, RejectsUnsupportedVersionBeforePublication) {
    const auto input =
        replaceOnce(validConfiguration(), "schema_version = 1", "schema_version = 2");

    const auto result = parseConfigurationToml(input);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
    EXPECT_NE(result.error().message.find("schema_version"), std::string::npos);
}

TEST(ConfigurationParserTest, RejectsUnknownKeysWithoutEchoingTheirValuesOrNames) {
    const auto input = replaceOnce(validConfiguration(), "profile = \"lustre\"",
                                   "profile = \"lustre\"\n"
                                   "private_key = \"super-secret-value\"");

    const auto result = parseConfigurationToml(input);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
    EXPECT_NE(result.error().message.find('$'), std::string::npos);
    EXPECT_EQ(result.error().message.find("private_key"), std::string::npos);
    EXPECT_EQ(result.error().message.find("super-secret-value"), std::string::npos);
    EXPECT_EQ(result.error().recovery.find("super-secret-value"), std::string::npos);
}

TEST(ConfigurationParserTest, RejectsUnknownNestedKeysAndWrongScalarTypes) {
    const auto unknown = replaceOnce(validConfiguration(), "accent = \"#4D7FFF\"",
                                     "accent = \"#4D7FFF\"\nsecret_token = \"hidden\"");
    const auto unknownResult = parseConfigurationToml(unknown);
    ASSERT_FALSE(unknownResult);
    EXPECT_EQ(unknownResult.error().code, ErrorCode::validation_error);
    EXPECT_NE(unknownResult.error().message.find("$.appearance"), std::string::npos);
    EXPECT_EQ(unknownResult.error().message.find("secret_token"), std::string::npos);

    const auto wrongType = replaceOnce(validConfiguration(), "size_px = 48", "size_px = 48.0");
    const auto typeResult = parseConfigurationToml(wrongType);
    ASSERT_FALSE(typeResult);
    EXPECT_EQ(typeResult.error().code, ErrorCode::validation_error);
    EXPECT_NE(typeResult.error().message.find("$.panel.size_px"), std::string::npos);
}

TEST(ConfigurationParserTest, RejectsInvalidRangesEnumsColorsAndNonFiniteNumbers) {
    const auto outOfRange =
        replaceOnce(validConfiguration(), "cursor_size_px = 24", "cursor_size_px = 257");
    EXPECT_FALSE(parseConfigurationToml(outOfRange));

    const auto invalidProfile =
        replaceOnce(validConfiguration(), "profile = \"lustre\"", "profile = \"Lustre\"");
    EXPECT_FALSE(parseConfigurationToml(invalidProfile));

    const auto invalidColor =
        replaceOnce(validConfiguration(), "accent = \"#4D7FFF\"", "accent = \"#GG7FFF\"");
    EXPECT_FALSE(parseConfigurationToml(invalidColor));

    const auto nonFinite =
        replaceOnce(validConfiguration(), "text_scale = 1.0", "text_scale = inf");
    EXPECT_FALSE(parseConfigurationToml(nonFinite));
}

TEST(ConfigurationParserTest, AcceptsIntegerSyntaxForJsonNumberFields) {
    auto input = replaceOnce(validConfiguration(), "text_scale = 1.0", "text_scale = 1");
    input = replaceOnce(std::move(input), "animation_scale = 1.0", "animation_scale = 1");

    const auto result = parseConfigurationToml(input);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value().appearance.textScale, 1.0);
    EXPECT_DOUBLE_EQ(result.value().accessibility.animationScale, 1.0);
}

TEST(ConfigurationParserTest, AppliesUnicodeCodePointStringLimits) {
    std::string acceptedUtf8;
    acceptedUtf8.reserve(std::size_t{128} * 2U);
    for (std::size_t index = 0; index < 128U; ++index) {
        acceptedUtf8 += "é";
    }
    const auto accepted = replaceOnce(validConfiguration(), "cursor_theme = \"default\"",
                                      "cursor_theme = \"" + acceptedUtf8 + "\"");
    EXPECT_TRUE(parseConfigurationToml(accepted));

    const auto rejected = replaceOnce(validConfiguration(), "cursor_theme = \"default\"",
                                      "cursor_theme = \"" + acceptedUtf8 + "é\"");
    const auto result = parseConfigurationToml(rejected);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
    EXPECT_NE(result.error().message.find("$.appearance.cursor_theme"), std::string::npos);
}

TEST(ConfigurationParserTest, RejectsOversizedAndDuplicateArrays) {
    const auto oversized = replaceOnce(validConfiguration(), "outputs = []", outputArray(33U));
    const auto oversizedResult = parseConfigurationToml(oversized);
    ASSERT_FALSE(oversizedResult);
    EXPECT_EQ(oversizedResult.error().code, ErrorCode::validation_error);

    const auto duplicate =
        replaceOnce(validConfiguration(), "outputs = []", "outputs = [\"DP-1\", \"DP-1\"]");
    const auto duplicateResult = parseConfigurationToml(duplicate);
    ASSERT_FALSE(duplicateResult);
    EXPECT_EQ(duplicateResult.error().code, ErrorCode::validation_error);
}

TEST(ConfigurationParserTest, ValidatesThenIgnoresDeveloperSettingsInProduction) {
    auto input = replaceOnce(validConfiguration(), "diagnostics_enabled = false",
                             "diagnostics_enabled = true");
    input = replaceOnce(std::move(input), "mock_capability_overrides = []",
                        "mock_capability_overrides = [\"secret-capability\"]");

    const auto production = parseConfigurationToml(input);
    ASSERT_TRUE(production);
    EXPECT_FALSE(production.value().developer.diagnosticsEnabled);
    EXPECT_TRUE(production.value().developer.mockCapabilityOverrides.empty());

    const auto requestedDeveloper = parseConfigurationToml(
        input, ConfigurationParseOptions{DeveloperSettingsPolicy::developer});
    ASSERT_TRUE(requestedDeveloper);
    if (!foundation::developerOverridesEnabled()) {
        EXPECT_FALSE(requestedDeveloper.value().developer.diagnosticsEnabled);
        EXPECT_TRUE(requestedDeveloper.value().developer.mockCapabilityOverrides.empty());
    } else {
        EXPECT_TRUE(requestedDeveloper.value().developer.diagnosticsEnabled);
        EXPECT_EQ(requestedDeveloper.value().developer.mockCapabilityOverrides,
                  std::vector<std::string>{"secret-capability"});
    }
}

} // namespace
} // namespace prismdrake::config
