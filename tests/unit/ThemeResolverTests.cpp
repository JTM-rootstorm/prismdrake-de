#include "ConfigurationParser.hpp"
#include "ThemeParser.hpp"
#include "ThemeResolver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace prismdrake::theme {
namespace {

[[nodiscard]] std::string readFixture(std::string_view relativePath) {
    const std::filesystem::path path = std::filesystem::path{PRISMDRAKE_SOURCE_DIR} / relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open committed resolver fixture");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string replaceOnce(std::string input, std::string_view before,
                                      std::string_view after) {
    const auto position = input.find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("resolver test replacement target is absent");
    }
    input.replace(position, before.size(), after);
    return input;
}

struct ThemeBundle final {
    ThemeDocument base;
    ThemeDocument lustre;
    ThemeDocument forge;
    ThemeDocument accessibility;
};

[[nodiscard]] ThemeBundle parseBundle() {
    auto base = parseThemeDocumentJson(readFixture("themes/base.tokens.json"));
    auto lustre = parseThemeDocumentJson(readFixture("themes/lustre.tokens.json"));
    auto forge = parseThemeDocumentJson(readFixture("themes/forge.tokens.json"));
    auto accessibility = parseThemeDocumentJson(readFixture("themes/accessibility.tokens.json"));
    if (!base || !lustre || !forge || !accessibility) {
        throw std::runtime_error("committed theme bundle did not parse");
    }
    return {std::move(base).value(), std::move(lustre).value(), std::move(forge).value(),
            std::move(accessibility).value()};
}

[[nodiscard]] config::Configuration parseConfiguration(std::string_view fixture) {
    auto parsed = config::parseConfigurationToml(readFixture(fixture));
    if (!parsed) {
        throw std::runtime_error("committed resolver configuration did not parse");
    }
    return std::move(parsed).value();
}

[[nodiscard]] config::Configuration parseConfigurationText(std::string_view input) {
    auto parsed = config::parseConfigurationToml(input);
    if (!parsed) {
        throw std::runtime_error("resolver test configuration did not parse");
    }
    return std::move(parsed).value();
}

[[nodiscard]] ThemeDocument parseThemeText(std::string_view input) {
    auto parsed = parseThemeDocumentJson(input);
    if (!parsed) {
        throw std::runtime_error("resolver test theme did not parse");
    }
    return std::move(parsed).value();
}

[[nodiscard]] bool allMaterialsUseFallback(const ResolvedMaterials &materials) {
    return materials.panel.usedFallback && materials.launcher.usedFallback &&
           materials.notification.usedFallback && materials.menu.usedFallback;
}

TEST(ThemeResolverTest, ResolvesCompleteLustreAndForgeCandidates) {
    const auto themes = parseBundle();
    const auto lustreConfig = parseConfiguration("examples/config/lustre.toml");
    const auto forgeConfig = parseConfiguration("examples/config/forge.toml");
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto lustre = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, lustreConfig, capabilities);
    const auto forge = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                             themes.accessibility, forgeConfig, capabilities);

    ASSERT_TRUE(lustre);
    ASSERT_TRUE(forge);
    EXPECT_EQ(lustre.value().profile, Profile::lustre);
    EXPECT_EQ(lustre.value().profileDisplayName, "Prismdrake Lustre");
    EXPECT_EQ(lustre.value().sources,
              (std::vector{ThemeSource::packaged_base, ThemeSource::packaged_lustre}));
    EXPECT_TRUE(lustre.value().primitive.colors.contains("paper"));
    EXPECT_TRUE(lustre.value().primitive.colors.contains("prism"));
    EXPECT_EQ(lustre.value().semantic.colors.selection.rgba, 0x4D7FFFFFU);
    EXPECT_TRUE(lustre.value().materials.panel.blurRequested);

    EXPECT_EQ(forge.value().profile, Profile::forge);
    EXPECT_EQ(forge.value().profileDisplayName, "Prismdrake Forge");
    EXPECT_EQ(forge.value().sources,
              (std::vector{ThemeSource::packaged_base, ThemeSource::packaged_forge,
                           ThemeSource::packaged_accessibility}));
    EXPECT_TRUE(forge.value().primitive.colors.contains("paper"));
    EXPECT_TRUE(forge.value().primitive.colors.contains("ember"));
    EXPECT_TRUE(allMaterialsUseFallback(forge.value().materials));
}

TEST(ThemeResolverTest, AppliesHighContrastWithoutCouplingIndependentPreferences) {
    const auto themes = parseBundle();
    const auto configuration = parseConfiguration("examples/config/accessible.toml");
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    const auto &resolved = result.value();
    EXPECT_TRUE(resolved.accessibility.highContrast);
    EXPECT_TRUE(resolved.accessibility.reducedMotion);
    EXPECT_TRUE(resolved.accessibility.transparencyDisabled);
    EXPECT_DOUBLE_EQ(resolved.semantic.typography.bodySizePx, 21.0);
    EXPECT_EQ(resolved.semantic.motion.fastMs, 0U);
    EXPECT_DOUBLE_EQ(resolved.accessibility.focusWidthPx, 4.0);
    EXPECT_DOUBLE_EQ(resolved.accessibility.minimumTargetSizePx, 48.0);
    EXPECT_GE(resolved.component.taskButton.borderWidthPx, 3.0);
    EXPECT_TRUE(allMaterialsUseFallback(resolved.materials));
    EXPECT_DOUBLE_EQ(resolved.materials.panel.opacity, 1.0);
    EXPECT_EQ(resolved.sources.back(), ThemeSource::packaged_accessibility);
    EXPECT_DOUBLE_EQ(resolved.accessibility.minimumContrastRatio, 7.0);
    ASSERT_FALSE(resolved.warnings.empty());
    EXPECT_EQ(resolved.warnings.front(), ThemeWarning::accent_suppressed_high_contrast);
}

TEST(ThemeResolverTest, AccessibilityPreferencesSurviveProfileSwitching) {
    const auto themes = parseBundle();
    const auto lustreConfiguration = parseConfiguration("examples/config/accessible.toml");
    const auto forgeConfiguration =
        parseConfigurationText(replaceOnce(readFixture("examples/config/accessible.toml"),
                                           "profile = \"lustre\"", "profile = \"forge\""));
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto lustre =
        resolveThemeCandidate(themes.base, themes.lustre, themes.forge, themes.accessibility,
                              lustreConfiguration, capabilities);
    const auto forge =
        resolveThemeCandidate(themes.base, themes.lustre, themes.forge, themes.accessibility,
                              forgeConfiguration, capabilities);

    ASSERT_TRUE(lustre);
    ASSERT_TRUE(forge);
    EXPECT_EQ(lustre.value().profile, Profile::lustre);
    EXPECT_EQ(forge.value().profile, Profile::forge);
    EXPECT_EQ(lustre.value().accessibility, forge.value().accessibility);
    EXPECT_EQ(lustre.value().semantic.colors, forge.value().semantic.colors);
    EXPECT_EQ(lustre.value().warnings, forge.value().warnings);
    EXPECT_EQ(lustre.value().sources.back(), ThemeSource::packaged_accessibility);
    EXPECT_EQ(forge.value().sources.back(), ThemeSource::packaged_accessibility);
    EXPECT_NE(lustre.value().semantic.spacing, forge.value().semantic.spacing);
}

TEST(ThemeResolverTest, AppliesCheckedScalingAndDisabledTransparencyIndependently) {
    const auto themes = parseBundle();
    auto input = replaceOnce(readFixture("examples/config/lustre.toml"), "text_scale = 1.0",
                             "text_scale = 1.25");
    input = replaceOnce(std::move(input), "animation_scale = 1.0", "animation_scale = 0.335");
    input = replaceOnce(std::move(input), "transparency_enabled = true",
                        "transparency_enabled = false");
    const auto configuration = parseConfigurationText(input);
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().accessibility.highContrast);
    EXPECT_FALSE(result.value().accessibility.reducedMotion);
    EXPECT_TRUE(result.value().accessibility.transparencyDisabled);
    EXPECT_DOUBLE_EQ(result.value().semantic.typography.bodySizePx, 17.5);
    EXPECT_DOUBLE_EQ(result.value().primitive.durationMs.at("fast"), 33.5);
    EXPECT_EQ(result.value().semantic.motion.fastMs, 34U);
    EXPECT_EQ(result.value().semantic.motion.normalMs, 60U);
    EXPECT_TRUE(allMaterialsUseFallback(result.value().materials));
    EXPECT_DOUBLE_EQ(result.value().materials.panel.opacity, 1.0);
}

TEST(ThemeResolverTest, StrongFocusUsesPackagedAccessibilityWithCompleteProvenance) {
    const auto themes = parseBundle();
    const auto configuration = parseConfigurationText(
        replaceOnce(readFixture("examples/config/lustre.toml"), "focus_emphasis = \"standard\"",
                    "focus_emphasis = \"strong\""));
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().accessibility.highContrast);
    EXPECT_DOUBLE_EQ(result.value().accessibility.focusWidthPx, 4.0);
    EXPECT_EQ(result.value().sources,
              (std::vector{ThemeSource::packaged_base, ThemeSource::packaged_lustre,
                           ThemeSource::packaged_accessibility}));
}

TEST(ThemeResolverTest, ConfigurationMinimumTargetDoesNotCoupleOtherAccessibilityModes) {
    const auto themes = parseBundle();
    const auto configuration = parseConfigurationText(
        replaceOnce(readFixture("examples/config/lustre.toml"), "minimum_target_size_px = 40",
                    "minimum_target_size_px = 80"));
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value().accessibility.minimumTargetSizePx, 80.0);
    EXPECT_FALSE(result.value().accessibility.highContrast);
    EXPECT_FALSE(result.value().accessibility.reducedMotion);
    EXPECT_EQ(result.value().sources.size(), 2U);
}

TEST(ThemeResolverTest, ReducedMotionAloneDoesNotEnableHighContrastOrOpaqueMaterials) {
    const auto themes = parseBundle();
    auto input = replaceOnce(readFixture("examples/config/lustre.toml"), "reduced_motion = false",
                             "reduced_motion = true");
    const auto configuration = parseConfigurationText(input);
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().accessibility.reducedMotion);
    EXPECT_FALSE(result.value().accessibility.highContrast);
    EXPECT_FALSE(result.value().accessibility.transparencyDisabled);
    EXPECT_EQ(result.value().semantic.motion.fastMs, 0U);
    EXPECT_FALSE(result.value().materials.panel.usedFallback);
    EXPECT_EQ(result.value().sources.size(), 3U);
}

TEST(ThemeResolverTest, BlurQualityOffSelectsFallbackWithoutDisablingTransparencyPreference) {
    const auto themes = parseBundle();
    const auto configuration = parseConfigurationText(
        replaceOnce(readFixture("examples/config/lustre.toml"), "blur_quality = \"balanced\"",
                    "blur_quality = \"off\""));
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_TRUE(allMaterialsUseFallback(result.value().materials));
    EXPECT_FALSE(result.value().accessibility.transparencyDisabled);
    EXPECT_EQ(result.value().sources.size(), 2U);
    EXPECT_TRUE(result.value().warnings.empty());
}

TEST(ThemeResolverTest, OpaqueOverrideForcesFallbackColorAlphaAndOpacity) {
    const auto themes = parseBundle();
    auto alphaProfile =
        replaceOnce(readFixture("themes/lustre.tokens.json"),
                    "\"kind\": \"opaque\", \"color\": \"#202A42FF\", \"opacity\": 1.0",
                    "\"kind\": \"alpha\", \"color\": \"#202A4200\", \"opacity\": 0.75");
    const auto alphaLustre = parseThemeText(alphaProfile);
    const auto normalConfiguration = parseConfiguration("examples/config/lustre.toml");
    constexpr ThemeResolveOptions missingBlur{{false, true}, false};
    const auto alphaFallback =
        resolveThemeCandidate(themes.base, alphaLustre, themes.forge, themes.accessibility,
                              normalConfiguration, missingBlur);
    ASSERT_TRUE(alphaFallback);
    EXPECT_EQ(alphaFallback.value().materials.panel.color.rgba, 0x202A4200U);
    EXPECT_DOUBLE_EQ(alphaFallback.value().materials.panel.opacity, 0.75);

    const auto configuration = parseConfigurationText(
        replaceOnce(readFixture("examples/config/lustre.toml"), "transparency_enabled = true",
                    "transparency_enabled = false"));
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, alphaLustre, themes.forge,
                                              themes.accessibility, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().materials.panel.usedFallback);
    EXPECT_EQ(result.value().materials.panel.color.rgba, 0x202A42FFU);
    EXPECT_DOUBLE_EQ(result.value().materials.panel.opacity, 1.0);
}

TEST(ThemeResolverTest, MissingBlurSelectsDeclaredFallbacksWithoutExecutingBlur) {
    const auto themes = parseBundle();
    const auto configuration = parseConfiguration("examples/config/lustre.toml");
    constexpr ThemeResolveOptions missingCapabilities{{false, false}, false};

    const auto result =
        resolveThemeCandidate(themes.base, themes.lustre, themes.forge, themes.accessibility,
                              configuration, missingCapabilities);

    ASSERT_TRUE(result);
    EXPECT_TRUE(allMaterialsUseFallback(result.value().materials));
    EXPECT_FALSE(result.value().materials.panel.blurRequested);
    EXPECT_EQ(result.value().materials.panel.color,
              themes.lustre.semantic.materials.panel.fallback.color);
    EXPECT_EQ(result.value().thumbnails, ThumbnailPresentation::application_icon_title_state);
    EXPECT_EQ(result.value().warnings, (std::vector{ThemeWarning::blur_fallback_active,
                                                    ThemeWarning::thumbnail_fallback_active}));
    EXPECT_EQ(result.value().capabilityFallbacks, themes.lustre.capabilityFallbacks);
}

TEST(ThemeResolverTest, RejectsHighContrastPaletteBelowDeclaredRatio) {
    const auto themes = parseBundle();
    const auto lowContrastAccessibility = parseThemeText(
        replaceOnce(readFixture("themes/accessibility.tokens.json"),
                    "\"focus_ring\": \"#FFF200FF\"", "\"focus_ring\": \"#151515FF\""));
    const auto configuration = parseConfiguration("examples/config/accessible.toml");
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result =
        resolveThemeCandidate(themes.base, themes.lustre, themes.forge, lowContrastAccessibility,
                              configuration, capabilities);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, foundation::ErrorCode::validation_error);
    EXPECT_NE(result.error().message.find("$.semantic.colors"), std::string::npos);

    const auto transparentStatus =
        parseThemeText(replaceOnce(readFixture("themes/accessibility.tokens.json"),
                                   "\"danger\": \"#FF7585FF\"", "\"danger\": \"#FF758500\""));
    const auto transparentResult = resolveThemeCandidate(
        themes.base, themes.lustre, themes.forge, transparentStatus, configuration, capabilities);
    ASSERT_FALSE(transparentResult);
    EXPECT_NE(transparentResult.error().message.find("$.semantic.colors"), std::string::npos);
}

TEST(ThemeResolverTest, PublishesOneEffectiveHighContrastMinimum) {
    const auto themes = parseBundle();
    const auto lowerAuthoredBorder = parseThemeText(
        replaceOnce(readFixture("themes/accessibility.tokens.json"),
                    "\"minimum_contrast_ratio\": 7.0", "\"minimum_contrast_ratio\": 1.0"));
    const auto configuration = parseConfiguration("examples/config/accessible.toml");
    constexpr ThemeResolveOptions capabilities{{true, true}, false};

    const auto result = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              lowerAuthoredBorder, configuration, capabilities);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value().accessibility.minimumContrastRatio, 7.0);
    EXPECT_DOUBLE_EQ(result.value().semantic.border.minimumContrastRatio, 7.0);
}

TEST(ThemeResolverTest, SafeModeAppliesLastAndIsDeterministic) {
    const auto themes = parseBundle();
    const auto configuration = parseConfiguration("examples/config/lustre.toml");
    constexpr ThemeResolveOptions safeMode{{true, true}, true};

    const auto first = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                             themes.accessibility, configuration, safeMode);
    const auto second = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                              themes.accessibility, configuration, safeMode);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    EXPECT_TRUE(allMaterialsUseFallback(first.value().materials));
    EXPECT_DOUBLE_EQ(first.value().materials.panel.opacity, 1.0);
    EXPECT_EQ(first.value().semantic.motion.normalMs, 0U);
    EXPECT_TRUE(first.value().accessibility.reducedMotion);
    EXPECT_EQ(first.value().warnings, (std::vector{ThemeWarning::safe_mode_active}));

    constexpr ThemeResolveOptions missingCapabilities{{false, false}, true};
    const auto fullyDegraded =
        resolveThemeCandidate(themes.base, themes.lustre, themes.forge, themes.accessibility,
                              configuration, missingCapabilities);
    ASSERT_TRUE(fullyDegraded);
    EXPECT_EQ(fullyDegraded.value().warnings, (std::vector{ThemeWarning::blur_fallback_active,
                                                           ThemeWarning::thumbnail_fallback_active,
                                                           ThemeWarning::safe_mode_active}));
}

TEST(ThemeResolverTest, RejectsMismatchedBundleWithoutMutatingPriorCandidate) {
    const auto themes = parseBundle();
    const auto configuration = parseConfiguration("examples/config/lustre.toml");
    constexpr ThemeResolveOptions capabilities{{true, true}, false};
    const auto previous = resolveThemeCandidate(themes.base, themes.lustre, themes.forge,
                                                themes.accessibility, configuration, capabilities);
    ASSERT_TRUE(previous);
    // Keep an independent value so a rejected candidate cannot make the assertion tautological.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto retained = previous.value();

    const auto rejected = resolveThemeCandidate(themes.base, themes.forge, themes.forge,
                                                themes.accessibility, configuration, capabilities);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(previous.value(), retained);
}

} // namespace
} // namespace prismdrake::theme
