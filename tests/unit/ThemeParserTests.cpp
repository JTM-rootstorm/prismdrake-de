#include "ThemeParser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace prismdrake::theme {
namespace {

using foundation::ErrorCode;

[[nodiscard]] std::string readFixture(std::string_view relativePath) {
    const std::filesystem::path path = std::filesystem::path{PRISMDRAKE_SOURCE_DIR} / relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open committed theme fixture");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string replaceOnce(std::string input, std::string_view before,
                                      std::string_view after) {
    const auto position = input.find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("theme test replacement target is absent");
    }
    input.replace(position, before.size(), after);
    return input;
}

TEST(ThemeParserTest, ParsesEveryCommittedCompleteDocument) {
    constexpr std::array fixtures{
        std::string_view{"themes/base.tokens.json"},
        std::string_view{"themes/lustre.tokens.json"},
        std::string_view{"themes/forge.tokens.json"},
        std::string_view{"themes/accessibility.tokens.json"},
    };

    for (const auto fixture : fixtures) {
        const auto result = parseThemeDocumentJson(readFixture(fixture));
        ASSERT_TRUE(result) << fixture << ": " << result.error().message;
        EXPECT_EQ(result.value().schemaVersion, 1U);
        EXPECT_FALSE(result.value().primitive.colors.empty());
    }
}

TEST(ThemeParserTest, NormalizesProfileIdentityAndTypedValues) {
    const auto result = parseThemeDocumentJson(readFixture("themes/lustre.tokens.json"));

    ASSERT_TRUE(result);
    const auto &theme = result.value();
    EXPECT_EQ(theme.layer, Layer::profile);
    EXPECT_EQ(theme.profile, std::optional{Profile::lustre});
    EXPECT_EQ(theme.profileDisplayName, std::optional{std::string{"Prismdrake Lustre"}});
    EXPECT_EQ(theme.semantic.colors.panelSurface.rgba, 0x202A42DEU);
    EXPECT_TRUE(theme.semantic.materials.panel.blurRequest.enabled);
    EXPECT_EQ(theme.semantic.materials.panel.fallback.kind, FallbackKind::opaque);
    EXPECT_DOUBLE_EQ(theme.semantic.materials.panel.fallback.opacity, 1.0);
    EXPECT_EQ(theme.semantic.motion.easing, MotionEasing::easeOut);
    EXPECT_DOUBLE_EQ(theme.accessibilityOverrides.minimumTargetSizePx, 44.0);
}

TEST(ThemeParserTest, EnforcesExactLayerIdentityAndGenerationModel) {
    const auto profileWithNullIdentity =
        replaceOnce(readFixture("themes/lustre.tokens.json"), "\"profile_id\": \"lustre\"",
                    "\"profile_id\": null");
    EXPECT_FALSE(parseThemeDocumentJson(profileWithNullIdentity));

    const auto mismatchedDisplay = replaceOnce(readFixture("themes/lustre.tokens.json"),
                                               "Prismdrake Lustre", "Prismdrake Forge");
    EXPECT_FALSE(parseThemeDocumentJson(mismatchedDisplay));

    const auto baseWithProfile = replaceOnce(readFixture("themes/base.tokens.json"),
                                             "\"profile_id\": null", "\"profile_id\": \"lustre\"");
    EXPECT_FALSE(parseThemeDocumentJson(baseWithProfile));

    const auto mutableGeneration =
        replaceOnce(readFixture("themes/base.tokens.json"), "immutable_snapshot", "mutable");
    EXPECT_FALSE(parseThemeDocumentJson(mutableGeneration));
}

TEST(ThemeParserTest, RejectsMalformedDuplicateAndUnsupportedDocuments) {
    const auto malformed = parseThemeDocumentJson("{");
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, ErrorCode::syntax_error);

    const auto duplicate = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"schema_version\": 1,",
                    "\"schema_version\": 1,\n  \"schema_version\": 1,"));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::syntax_error);

    const auto nestedDuplicate = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"force_opaque\": true",
                    "\"force_opaque\": true, \"force_opaque\": true"));
    ASSERT_FALSE(nestedDuplicate);
    EXPECT_EQ(nestedDuplicate.error().code, ErrorCode::syntax_error);

    const auto commented = parseThemeDocumentJson(replaceOnce(
        readFixture("themes/base.tokens.json"), "{\n", "{\n  // comments are not JSON\n"));
    ASSERT_FALSE(commented);
    EXPECT_EQ(commented.error().code, ErrorCode::syntax_error);

    const auto unsupported = parseThemeDocumentJson(replaceOnce(
        readFixture("themes/base.tokens.json"), "\"schema_version\": 1", "\"schema_version\": 2"));
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::unsupported);

    std::string invalidUtf8 = readFixture("themes/base.tokens.json");
    invalidUtf8.insert(invalidUtf8.find("sans-serif"), 1U, static_cast<char>(0xff));
    const auto malformedEncoding = parseThemeDocumentJson(invalidUtf8);
    ASSERT_FALSE(malformedEncoding);
    EXPECT_EQ(malformedEncoding.error().code, ErrorCode::syntax_error);
}

TEST(ThemeParserTest, UsesJsonSchemaMathematicalIntegerSemantics) {
    auto integralForms = replaceOnce(readFixture("themes/base.tokens.json"),
                                     "\"schema_version\": 1", "\"schema_version\": 1e0");
    integralForms =
        replaceOnce(std::move(integralForms), "\"weight_normal\": 400", "\"weight_normal\": 400.0");
    integralForms = replaceOnce(std::move(integralForms), "\"weight_emphasis\": 600",
                                "\"weight_emphasis\": 6e2");
    EXPECT_TRUE(parseThemeDocumentJson(integralForms));

    const auto fractionalInteger =
        replaceOnce(readFixture("themes/base.tokens.json"), "\"weight_normal\": 400",
                    "\"weight_normal\": 400.5");
    EXPECT_FALSE(parseThemeDocumentJson(fractionalInteger));

    const auto overflowingNumber = replaceOnce(readFixture("themes/base.tokens.json"),
                                               "\"body_size_px\": 14", "\"body_size_px\": 1e999");
    EXPECT_FALSE(parseThemeDocumentJson(overflowingNumber));
}

TEST(ThemeParserTest, RejectsMissingUnknownAndWrongTypeWithoutEchoingInput) {
    const auto missing = parseThemeDocumentJson(replaceOnce(
        readFixture("themes/base.tokens.json"), ",\n      \"success\": \"#55C98AFF\"", ""));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::validation_error);
    EXPECT_NE(missing.error().message.find("$.semantic.colors.success"), std::string::npos);

    const auto unknownInput =
        replaceOnce(readFixture("themes/base.tokens.json"), "\"schema_version\": 1,",
                    "\"schema_version\": 1,\n  \"private_token\": \"super-secret-value\",");
    const auto unknown = parseThemeDocumentJson(unknownInput);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::validation_error);
    EXPECT_EQ(unknown.error().message.find("private_token"), std::string::npos);
    EXPECT_EQ(unknown.error().message.find("super-secret-value"), std::string::npos);
    EXPECT_EQ(unknown.error().recovery.find("super-secret-value"), std::string::npos);

    const auto wrongType = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"minimum_target_size_px\": 44",
                    "\"minimum_target_size_px\": \"44\""));
    ASSERT_FALSE(wrongType);
    EXPECT_EQ(wrongType.error().code, ErrorCode::validation_error);
}

TEST(ThemeParserTest, RejectsInvalidRangesColorsKeysAndFallbacks) {
    EXPECT_FALSE(parseThemeDocumentJson(replaceOnce(readFixture("themes/lustre.tokens.json"),
                                                    "\"radius_px\": 28", "\"radius_px\": 129")));
    EXPECT_FALSE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/lustre.tokens.json"), "#202A42DE", "#GG2A42DE")));
    EXPECT_FALSE(
        parseThemeDocumentJson(replaceOnce(readFixture("themes/lustre.tokens.json"),
                                           "\"kind\": \"opaque\"", "\"kind\": \"capture\"")));
    EXPECT_FALSE(parseThemeDocumentJson(replaceOnce(readFixture("themes/lustre.tokens.json"),
                                                    "\"fallback\": {", "\"missing\": {")));
    EXPECT_FALSE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"fast\": 90", "\"Bad Key\": 90")));
    EXPECT_FALSE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"width_px\": 2", "\"extra_px\": 2")));
    EXPECT_TRUE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"xs\": 4", "\"xs\": 65535")));
    EXPECT_FALSE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"xs\": 4", "\"xs\": 65536")));
    EXPECT_FALSE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"width_px\": 2", "\"width_px\": 0")));
    EXPECT_FALSE(parseThemeDocumentJson(replaceOnce(readFixture("themes/lustre.tokens.json"),
                                                    "\"opacity\": 1.0 }", "\"opacity\": 0.75 }")));
    EXPECT_FALSE(parseThemeDocumentJson(replaceOnce(readFixture("themes/lustre.tokens.json"),
                                                    "#202A42FF\", \"opacity\": 1.0",
                                                    "#202A4200\", \"opacity\": 1.0")));
    EXPECT_FALSE(
        parseThemeDocumentJson(replaceOnce(readFixture("themes/base.tokens.json"),
                                           "\"duration_scale\": 0.0", "\"duration_scale\": 0.5")));
    EXPECT_FALSE(
        parseThemeDocumentJson(replaceOnce(readFixture("themes/base.tokens.json"),
                                           "\"force_opaque\": true", "\"force_opaque\": false")));
}

TEST(ThemeParserTest, EnforcesExactMetricGroupsAndRedactsTraversalAttempts) {
    struct MissingMetricCase final {
        std::string_view fragment;
        std::string_view expectedPath;
    };
    constexpr std::array missingMetricCases{
        MissingMetricCase{"\"unit_px\": 4, ", "$.semantic.spacing.unit_px"},
        MissingMetricCase{"\"height_px\": 48, ", "$.semantic.panel.height_px"},
        MissingMetricCase{"\"titlebar_height_px\": 34, ",
                          "$.semantic.decoration.titlebar_height_px"},
        MissingMetricCase{"\"small_px\": 16, ", "$.semantic.icon.small_px"},
        MissingMetricCase{"\"width_px\": 2, ", "$.semantic.focus.width_px"},
        MissingMetricCase{"\"minimum_px\": 40", "$.semantic.targets.minimum_px"},
    };
    for (const auto &testCase : missingMetricCases) {
        const auto result = parseThemeDocumentJson(replaceOnce(
            readFixture("themes/base.tokens.json"), testCase.fragment, std::string_view{}));
        ASSERT_FALSE(result) << testCase.expectedPath;
        EXPECT_NE(result.error().message.find(testCase.expectedPath), std::string::npos);
    }

    const auto extraFocus = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"offset_px\": 2",
                    "\"offset_px\": 2, \"secret_width_px\": 999"));
    ASSERT_FALSE(extraFocus);
    EXPECT_EQ(extraFocus.error().message.find("secret_width_px"), std::string::npos);
    EXPECT_EQ(extraFocus.error().recovery.find("999"), std::string::npos);

    const auto traversal = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"schema_version\": 1,",
                    "\"schema_version\": 1, \"asset_path\": \"../../super-secret\","));
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().message.find("asset_path"), std::string::npos);
    EXPECT_EQ(traversal.error().message.find("super-secret"), std::string::npos);
    EXPECT_EQ(traversal.error().recovery.find("../../"), std::string::npos);
}

TEST(ThemeParserTest, EnforcesInputArrayNestingAndUnicodeBounds) {
    std::string exact = readFixture("themes/base.tokens.json") + " ";
    ASSERT_LT(exact.size(), maximumThemeDocumentBytes);
    exact.append(maximumThemeDocumentBytes - exact.size(), ' ');
    ASSERT_EQ(exact.size(), maximumThemeDocumentBytes);
    EXPECT_TRUE(parseThemeDocumentJson(exact));
    exact.push_back(' ');
    const auto tooLarge = parseThemeDocumentJson(exact);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, ErrorCode::too_large);

    std::string maximumFamily;
    for (std::size_t index = 0; index < maximumThemeStringCodePoints; ++index) {
        maximumFamily += "é";
    }
    EXPECT_TRUE(parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "sans-serif", maximumFamily)));

    const std::string longFamily = maximumFamily + "é";
    const auto longString =
        replaceOnce(readFixture("themes/base.tokens.json"), "sans-serif", longFamily);
    EXPECT_FALSE(parseThemeDocumentJson(longString));

    std::string manyFamilies = "\"font_families\": [";
    for (std::size_t index = 0; index < maximumThemeArrayItems + 1U; ++index) {
        if (index != 0U) {
            manyFamilies += ',';
        }
        manyFamilies += "\"sans-serif\"";
    }
    manyFamilies += ']';
    const auto oversizedArray = replaceOnce(readFixture("themes/base.tokens.json"),
                                            "\"font_families\": [\"sans-serif\"]", manyFamilies);
    EXPECT_FALSE(parseThemeDocumentJson(oversizedArray));

    std::string nestedValue = "0";
    for (std::size_t depth = 0; depth <= maximumThemeNesting; ++depth) {
        nestedValue.insert(nestedValue.begin(), '[');
        nestedValue.push_back(']');
    }
    const auto overlyNested = replaceOnce(readFixture("themes/base.tokens.json"),
                                          "\"blur\": \"Use each material fallback without "
                                          "shell-side capture.\"",
                                          "\"blur\": " + nestedValue);
    EXPECT_FALSE(parseThemeDocumentJson(overlyNested));

    std::string manyNodes = "[";
    for (std::size_t outer = 0U; outer < 64U; ++outer) {
        if (outer != 0U) {
            manyNodes += ',';
        }
        manyNodes += '[';
        for (std::size_t inner = 0U; inner < 64U; ++inner) {
            if (inner != 0U) {
                manyNodes += ',';
            }
            manyNodes += '0';
        }
        manyNodes += ']';
    }
    manyNodes += ']';
    const auto excessiveNodes = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"schema_version\": 1,",
                    "\"schema_version\": 1, \"bounded_adversary\": " + manyNodes + ','));
    ASSERT_FALSE(excessiveNodes);
    EXPECT_EQ(excessiveNodes.error().code, ErrorCode::validation_error);

    std::string manyEntries = "{";
    for (std::size_t index = 0U; index < maximumThemeObjectEntries + 1U; ++index) {
        if (index != 0U) {
            manyEntries += ',';
        }
        manyEntries += "\"entry_" + std::to_string(index) + "\":0";
    }
    manyEntries += '}';
    const auto excessiveEntries = parseThemeDocumentJson(
        replaceOnce(readFixture("themes/base.tokens.json"), "\"schema_version\": 1,",
                    "\"schema_version\": 1, \"bounded_object\": " + manyEntries + ','));
    ASSERT_FALSE(excessiveEntries);
    EXPECT_EQ(excessiveEntries.error().code, ErrorCode::validation_error);
}

} // namespace
} // namespace prismdrake::theme
