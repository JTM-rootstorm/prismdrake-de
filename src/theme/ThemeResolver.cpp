#include "ThemeResolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace prismdrake::theme {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error resolutionError(std::string_view path, std::string_view recovery) {
    return {ErrorCode::validation_error, "Theme resolution failed at " + std::string(path) + '.',
            std::string(recovery)};
}

[[nodiscard]] bool matchesLayer(const ThemeDocument &document, Layer layer) noexcept {
    return document.schemaVersion == 1U && document.layer == layer;
}

[[nodiscard]] Result<void> validateBundle(const ThemeDocument &base, const ThemeDocument &lustre,
                                          const ThemeDocument &forge,
                                          const ThemeDocument &accessibility) {
    if (!matchesLayer(base, Layer::base) || base.profile.has_value() ||
        base.profileDisplayName.has_value()) {
        return Result<void>::failure(resolutionError(
            "$.base", "Supply the complete packaged base theme with null profile identity."));
    }
    if (!matchesLayer(accessibility, Layer::accessibility) || accessibility.profile.has_value() ||
        accessibility.profileDisplayName.has_value()) {
        return Result<void>::failure(resolutionError(
            "$.accessibility",
            "Supply the complete packaged accessibility theme with null profile identity."));
    }
    if (!matchesLayer(lustre, Layer::profile) || lustre.profile != Profile::lustre ||
        lustre.profileDisplayName != std::optional<std::string>{"Prismdrake Lustre"}) {
        return Result<void>::failure(resolutionError(
            "$.lustre", "Supply the exact packaged Prismdrake Lustre profile document."));
    }
    if (!matchesLayer(forge, Layer::profile) || forge.profile != Profile::forge ||
        forge.profileDisplayName != std::optional<std::string>{"Prismdrake Forge"}) {
        return Result<void>::failure(resolutionError(
            "$.forge", "Supply the exact packaged Prismdrake Forge profile document."));
    }
    return Result<void>::success();
}

[[nodiscard]] std::optional<std::uint8_t> hexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] Result<Color> configurationAccent(std::string_view accent) {
    if (accent.size() != 7U || accent.front() != '#') {
        return Result<Color>::failure(resolutionError(
            "$.configuration.appearance.accent", "Use a validated #RRGGBB configuration accent."));
    }
    std::uint32_t packed = 0U;
    for (std::size_t index = 1U; index < accent.size(); ++index) {
        const auto nibble = hexNibble(accent[index]);
        if (!nibble.has_value()) {
            return Result<Color>::failure(
                resolutionError("$.configuration.appearance.accent",
                                "Use a validated #RRGGBB configuration accent."));
        }
        packed = (packed << 4U) | nibble.value();
    }
    return Result<Color>::success(Color{(packed << 8U) | 0xffU});
}

template <typename Value>
void overlayMap(std::map<std::string, Value, std::less<>> &destination,
                const std::map<std::string, Value, std::less<>> &overlay) {
    for (const auto &[key, value] : overlay) {
        destination.erase(key);
        destination.emplace(key, value);
    }
}

[[nodiscard]] Result<NumberMap> scaleMap(const NumberMap &input, double scale,
                                         std::string_view path) {
    NumberMap output;
    for (const auto &[key, value] : input) {
        const double scaled = value * scale;
        if (!std::isfinite(scaled) || scaled < 0.0) {
            return Result<NumberMap>::failure(resolutionError(
                path, "Use values that remain finite after accessibility scaling."));
        }
        output.emplace(key, scaled);
    }
    return Result<NumberMap>::success(std::move(output));
}

[[nodiscard]] Result<PrimitiveTokens> resolvePrimitive(const PrimitiveTokens &base,
                                                       const PrimitiveTokens &profile,
                                                       const config::Configuration &configuration,
                                                       bool suppressAccent, double durationScale) {
    ColorMap colors = base.colors;
    NumberMap spacing = base.spacingPx;
    NumberMap fontSizes = base.fontSizePx;
    NumberMap durations = base.durationMs;
    NumberMap radii = base.radiusPx;
    NumberMap opacity = base.opacity;
    overlayMap(colors, profile.colors);
    overlayMap(spacing, profile.spacingPx);
    overlayMap(fontSizes, profile.fontSizePx);
    overlayMap(durations, profile.durationMs);
    overlayMap(radii, profile.radiusPx);
    overlayMap(opacity, profile.opacity);

    if (!suppressAccent) {
        auto accent = configurationAccent(configuration.appearance.accent);
        if (!accent) {
            return Result<PrimitiveTokens>::failure(accent.error());
        }
        colors.erase("accent");
        colors.emplace("accent", accent.value());
    }

    auto scaledFonts =
        scaleMap(fontSizes, configuration.appearance.textScale, "$.primitive.font_size_px");
    if (!scaledFonts) {
        return Result<PrimitiveTokens>::failure(scaledFonts.error());
    }
    auto scaledDurations = scaleMap(durations, durationScale, "$.primitive.duration_ms");
    if (!scaledDurations) {
        return Result<PrimitiveTokens>::failure(scaledDurations.error());
    }

    return Result<PrimitiveTokens>::success(PrimitiveTokens{
        std::move(colors), std::move(spacing), profile.fontFamilies, std::move(scaledFonts).value(),
        std::move(scaledDurations).value(), std::move(radii), std::move(opacity)});
}

[[nodiscard]] SemanticColors withSelection(const SemanticColors &colors, Color selection) {
    return {colors.panelSurface,   colors.elevatedSurface, colors.windowFrame, colors.borderActive,
            colors.borderInactive, colors.textPrimary,     colors.textMuted,   selection,
            colors.focusRing,      colors.danger,          colors.warning,     colors.success};
}

[[nodiscard]] bool highContrastStatesRemainDistinct(const SemanticColors &colors) noexcept {
    const std::array values{colors.borderActive.rgba, colors.borderInactive.rgba,
                            colors.selection.rgba,    colors.focusRing.rgba,
                            colors.danger.rgba,       colors.warning.rgba,
                            colors.success.rgba};
    for (std::size_t left = 0U; left < values.size(); ++left) {
        for (std::size_t right = left + 1U; right < values.size(); ++right) {
            if (values[left] == values[right]) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] double linearColorChannel(std::uint32_t rgba, unsigned int shift) noexcept {
    const double encoded = static_cast<double>((rgba >> shift) & 0xffU) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double relativeLuminance(Color color) noexcept {
    return (0.2126 * linearColorChannel(color.rgba, 24U)) +
           (0.7152 * linearColorChannel(color.rgba, 16U)) +
           (0.0722 * linearColorChannel(color.rgba, 8U));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] double contrastRatio(Color left, Color right) noexcept {
    const double leftLuminance = relativeLuminance(left);
    const double rightLuminance = relativeLuminance(right);
    const double lighter = std::max(leftLuminance, rightLuminance);
    const double darker = std::min(leftLuminance, rightLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

[[nodiscard]] bool highContrastPaletteMeetsRatio(const SemanticColors &colors,
                                                 double minimumRatio) noexcept {
    const std::array foregrounds{
        colors.textPrimary, colors.textMuted,    colors.selection,
        colors.focusRing,   colors.borderActive, colors.borderInactive,
        colors.danger,      colors.warning,      colors.success,
    };
    const std::array backgrounds{colors.panelSurface, colors.elevatedSurface};
    for (const auto foreground : foregrounds) {
        if ((foreground.rgba & 0xffU) != 0xffU) {
            return false;
        }
        for (const auto background : backgrounds) {
            if ((background.rgba & 0xffU) != 0xffU ||
                contrastRatio(foreground, background) < minimumRatio) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Result<Motion> scaleMotion(const Motion &motion, double scale) {
    const auto scaledDuration = [scale](std::uint16_t value) -> std::optional<std::uint16_t> {
        const double scaled = static_cast<double>(value) * scale;
        if (!std::isfinite(scaled) || scaled < 0.0 ||
            scaled > static_cast<double>(std::numeric_limits<std::uint16_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::lround(scaled));
    };
    const auto fast = scaledDuration(motion.fastMs);
    const auto normal = scaledDuration(motion.normalMs);
    if (!fast || !normal) {
        return Result<Motion>::failure(resolutionError(
            "$.semantic.motion", "Use motion values that remain in range after scaling."));
    }
    return Result<Motion>::success(Motion{*fast, *normal, motion.easing});
}

[[nodiscard]] Result<SemanticTokens> resolveSemantic(const ThemeDocument &profile,
                                                     const ThemeDocument &accessibility,
                                                     const config::Configuration &configuration,
                                                     double durationScale, bool suppressAccent) {
    const bool highContrast = configuration.appearance.highContrast;
    const SemanticColors &selectedColors =
        highContrast ? accessibility.semantic.colors : profile.semantic.colors;
    const double effectiveContrast =
        std::max(accessibility.accessibilityOverrides.highContrast.minimumContrastRatio,
                 accessibility.semantic.border.minimumContrastRatio);
    if (highContrast && (!highContrastStatesRemainDistinct(selectedColors) ||
                         !highContrastPaletteMeetsRatio(selectedColors, effectiveContrast))) {
        return Result<SemanticTokens>::failure(resolutionError(
            "$.semantic.colors",
            "Keep opaque text, selection, focus, border, and status colors visibly distinct at "
            "the declared contrast ratio."));
    }
    auto accent = configurationAccent(configuration.appearance.accent);
    if (!suppressAccent && !accent) {
        return Result<SemanticTokens>::failure(accent.error());
    }
    SemanticColors colors =
        suppressAccent ? selectedColors : withSelection(selectedColors, accent.value());

    const auto &authored = profile.semantic;
    const Materials &materials =
        highContrast ? accessibility.semantic.materials : authored.materials;
    const Border &authoredBorder = highContrast ? accessibility.semantic.border : authored.border;
    const Border border{authoredBorder.thicknessPx,
                        highContrast ? effectiveContrast : authoredBorder.minimumContrastRatio};

    const double bodySize = authored.typography.bodySizePx * configuration.appearance.textScale;
    const double titleSize = authored.typography.titleSizePx * configuration.appearance.textScale;
    if (!std::isfinite(bodySize) || !std::isfinite(titleSize)) {
        return Result<SemanticTokens>::failure(resolutionError(
            "$.semantic.typography", "Use font sizes that remain finite after text scaling."));
    }
    const Typography typography{authored.typography.bodyFamily, bodySize, titleSize,
                                authored.typography.weightNormal,
                                authored.typography.weightEmphasis};

    NumberMap focus = highContrast ? accessibility.semantic.focus : authored.focus;
    const auto focusIterator = focus.find("width_px");
    if (focusIterator == focus.end()) {
        return Result<SemanticTokens>::failure(resolutionError(
            "$.semantic.focus.width_px", "Declare the required focus width token."));
    }
    double focusWidth = focusIterator->second;
    if (configuration.accessibility.focusEmphasis == config::FocusEmphasis::strong) {
        focusWidth =
            std::max(focusWidth, accessibility.accessibilityOverrides.highContrast.focusWidthPx);
    }
    if (highContrast) {
        focusWidth =
            std::max(focusWidth, accessibility.accessibilityOverrides.highContrast.focusWidthPx);
    }
    focus.insert_or_assign("width_px", focusWidth);

    NumberMap targets = authored.targets;
    const auto targetIterator = targets.find("minimum_px");
    if (targetIterator == targets.end()) {
        return Result<SemanticTokens>::failure(resolutionError(
            "$.semantic.targets.minimum_px", "Declare the required minimum target token."));
    }
    double minimumTarget =
        std::max({targetIterator->second, profile.accessibilityOverrides.minimumTargetSizePx,
                  static_cast<double>(configuration.accessibility.minimumTargetSizePx)});
    if (highContrast) {
        const auto accessibleTarget = accessibility.semantic.targets.find("minimum_px");
        if (accessibleTarget == accessibility.semantic.targets.end()) {
            return Result<SemanticTokens>::failure(
                resolutionError("$.accessibility.semantic.targets.minimum_px",
                                "Declare the required accessibility target token."));
        }
        minimumTarget = std::max({minimumTarget, accessibleTarget->second,
                                  accessibility.accessibilityOverrides.minimumTargetSizePx});
    }
    targets.insert_or_assign("minimum_px", minimumTarget);

    auto motion = scaleMotion(authored.motion, durationScale);
    if (!motion) {
        return Result<SemanticTokens>::failure(motion.error());
    }

    return Result<SemanticTokens>::success(
        SemanticTokens{std::move(colors), materials, border, authored.shadow, typography,
                       authored.spacing, authored.panel, authored.decoration, authored.icon,
                       std::move(focus), std::move(motion).value(), std::move(targets)});
}

[[nodiscard]] ComponentStyle accessibleComponent(const ComponentStyle &profile,
                                                 const ComponentStyle &accessibility,
                                                 bool highContrast) {
    return {profile.radiusPx, profile.paddingPx,
            highContrast ? std::max(profile.borderWidthPx, accessibility.borderWidthPx)
                         : profile.borderWidthPx};
}

[[nodiscard]] ComponentTokens resolveComponents(const ComponentTokens &profile,
                                                const ComponentTokens &accessibility,
                                                bool highContrast) {
    return {
        accessibleComponent(profile.taskButton, accessibility.taskButton, highContrast),
        accessibleComponent(profile.launcherTile, accessibility.launcherTile, highContrast),
        accessibleComponent(profile.titlebarButton, accessibility.titlebarButton, highContrast),
        accessibleComponent(profile.notificationCard, accessibility.notificationCard, highContrast),
        accessibleComponent(profile.quickSetting, accessibility.quickSetting, highContrast),
        accessibleComponent(profile.tooltip, accessibility.tooltip, highContrast),
        accessibleComponent(profile.menuItem, accessibility.menuItem, highContrast),
    };
}

[[nodiscard]] ResolvedMaterial resolveMaterial(const Material &material, bool useFallback,
                                               bool forceOpaque, bool blurAvailable) {
    if (useFallback) {
        const Color color =
            forceOpaque ? Color{material.fallback.color.rgba | 0xffU} : material.fallback.color;
        return {color, forceOpaque ? 1.0 : material.fallback.opacity, false, 0.0, 1.0, true};
    }
    return {material.tint,
            material.opacity,
            blurAvailable && material.blurRequest.enabled,
            blurAvailable && material.blurRequest.enabled ? material.blurRequest.radiusPx : 0.0,
            blurAvailable && material.blurRequest.enabled ? material.blurRequest.saturation : 1.0,
            false};
}

[[nodiscard]] ResolvedMaterials resolveMaterials(const Materials &materials,
                                                 const config::Configuration &configuration,
                                                 ThemeResolveOptions options, bool forceOpaque) {
    const bool transparencyDisabled =
        !configuration.appearance.transparencyEnabled || options.safeMode;
    const bool blurDisabled = configuration.appearance.blurQuality == config::BlurQuality::off;
    const bool useFallback = transparencyDisabled || blurDisabled ||
                             !options.capabilities.blurAvailable || options.safeMode;
    return {
        resolveMaterial(materials.panel, useFallback, forceOpaque,
                        options.capabilities.blurAvailable),
        resolveMaterial(materials.launcher, useFallback, forceOpaque,
                        options.capabilities.blurAvailable),
        resolveMaterial(materials.notification, useFallback, forceOpaque,
                        options.capabilities.blurAvailable),
        resolveMaterial(materials.menu, useFallback, forceOpaque,
                        options.capabilities.blurAvailable),
    };
}

[[nodiscard]] bool anyBlurRequest(const Materials &materials) noexcept {
    return materials.panel.blurRequest.enabled || materials.launcher.blurRequest.enabled ||
           materials.notification.blurRequest.enabled || materials.menu.blurRequest.enabled;
}

} // namespace

Result<ResolvedThemeCandidate>
resolveThemeCandidate(const ThemeDocument &base, const ThemeDocument &lustre,
                      const ThemeDocument &forge, const ThemeDocument &accessibility,
                      const config::Configuration &configuration, ThemeResolveOptions options) {
    auto bundle = validateBundle(base, lustre, forge, accessibility);
    if (!bundle) {
        return Result<ResolvedThemeCandidate>::failure(bundle.error());
    }

    const ThemeDocument &profile =
        configuration.profile == config::Profile::lustre ? lustre : forge;
    const Profile profileId =
        configuration.profile == config::Profile::lustre ? Profile::lustre : Profile::forge;
    if (profile.profile != profileId || !profile.profileDisplayName.has_value()) {
        return Result<ResolvedThemeCandidate>::failure(resolutionError(
            "$.profile", "Select the packaged profile matching the validated configuration."));
    }

    const bool highContrast = configuration.appearance.highContrast;
    const bool reducedMotionRequested = configuration.appearance.reducedMotion;
    const bool reducedMotion = reducedMotionRequested || options.safeMode;
    const bool transparencyOverrideRequested = !configuration.appearance.transparencyEnabled;
    const bool transparencyDisabled = transparencyOverrideRequested || options.safeMode;
    const bool strongFocus =
        configuration.accessibility.focusEmphasis == config::FocusEmphasis::strong;
    const bool usesAccessibilityLayer =
        highContrast || reducedMotionRequested || transparencyOverrideRequested || strongFocus;
    const bool suppressAccent = highContrast;
    const double durationScale =
        options.safeMode         ? 0.0
        : reducedMotionRequested ? accessibility.accessibilityOverrides.reducedMotion.durationScale
                                 : configuration.accessibility.animationScale;
    const bool forceOpaque =
        options.safeMode || (transparencyOverrideRequested &&
                             accessibility.accessibilityOverrides.transparencyDisabled.forceOpaque);

    auto primitive = resolvePrimitive(base.primitive, profile.primitive, configuration,
                                      suppressAccent, durationScale);
    if (!primitive) {
        return Result<ResolvedThemeCandidate>::failure(primitive.error());
    }
    auto semantic =
        resolveSemantic(profile, accessibility, configuration, durationScale, suppressAccent);
    if (!semantic) {
        return Result<ResolvedThemeCandidate>::failure(semantic.error());
    }
    auto components = resolveComponents(profile.component, accessibility.component, highContrast);
    auto materials =
        resolveMaterials(semantic.value().materials, configuration, options, forceOpaque);

    std::vector<ThemeSource> sources{ThemeSource::packaged_base, profileId == Profile::lustre
                                                                     ? ThemeSource::packaged_lustre
                                                                     : ThemeSource::packaged_forge};
    if (usesAccessibilityLayer) {
        sources.push_back(ThemeSource::packaged_accessibility);
    }

    std::vector<ThemeWarning> warnings;
    if (suppressAccent) {
        warnings.push_back(ThemeWarning::accent_suppressed_high_contrast);
    }
    if (!options.capabilities.blurAvailable && anyBlurRequest(semantic.value().materials)) {
        warnings.push_back(ThemeWarning::blur_fallback_active);
    }
    if (!options.capabilities.thumbnailsAvailable) {
        warnings.push_back(ThemeWarning::thumbnail_fallback_active);
    }
    if (options.safeMode) {
        warnings.push_back(ThemeWarning::safe_mode_active);
    }

    const double focusWidth = semantic.value().focus.at("width_px");
    const double minimumTarget = semantic.value().targets.at("minimum_px");
    const double minimumContrast =
        highContrast
            ? std::max(accessibility.semantic.border.minimumContrastRatio,
                       accessibility.accessibilityOverrides.highContrast.minimumContrastRatio)
            : semantic.value().border.minimumContrastRatio;
    return Result<ResolvedThemeCandidate>::success(ResolvedThemeCandidate{
        1U,
        profileId,
        *profile.profileDisplayName,
        std::move(sources),
        std::move(primitive).value(),
        std::move(semantic).value(),
        std::move(components),
        EffectiveAccessibility{highContrast, reducedMotion, transparencyDisabled,
                               configuration.appearance.textScale, durationScale, focusWidth,
                               minimumTarget, minimumContrast},
        highContrast ? accessibility.capabilityFallbacks : profile.capabilityFallbacks,
        std::move(materials),
        options.capabilities.thumbnailsAvailable
            ? ThumbnailPresentation::provided_thumbnail
            : ThumbnailPresentation::application_icon_title_state,
        std::move(warnings),
    });
}

} // namespace prismdrake::theme
