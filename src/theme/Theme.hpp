#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::theme {

enum class Layer : std::uint8_t { base, profile, accessibility };
enum class Profile : std::uint8_t { lustre, forge };
enum class FallbackKind : std::uint8_t { alpha, opaque };
enum class MotionEasing : std::uint8_t { linear, easeOut, easeInOut, crisp };

/// Packed red, green, blue, and alpha channels from a schema-valid #RRGGBBAA value.
struct Color final {
    const std::uint32_t rgba;

    friend bool operator==(const Color &, const Color &) = default;
};

using ColorMap = std::map<std::string, Color, std::less<>>;
using NumberMap = std::map<std::string, double, std::less<>>;

struct PrimitiveTokens final {
    const ColorMap colors;
    const NumberMap spacingPx;
    const std::vector<std::string> fontFamilies;
    const NumberMap fontSizePx;
    const NumberMap durationMs;
    const NumberMap radiusPx;
    const NumberMap opacity;

    friend bool operator==(const PrimitiveTokens &, const PrimitiveTokens &) = default;
};

struct BlurRequest final {
    const bool enabled;
    const double radiusPx;
    const double saturation;

    friend bool operator==(const BlurRequest &, const BlurRequest &) = default;
};

struct MaterialFallback final {
    const FallbackKind kind;
    const Color color;
    const double opacity;

    friend bool operator==(const MaterialFallback &, const MaterialFallback &) = default;
};

struct Material final {
    const Color tint;
    const double opacity;
    const BlurRequest blurRequest;
    const MaterialFallback fallback;

    friend bool operator==(const Material &, const Material &) = default;
};

struct SemanticColors final {
    const Color panelSurface;
    const Color elevatedSurface;
    const Color windowFrame;
    const Color borderActive;
    const Color borderInactive;
    const Color textPrimary;
    const Color textMuted;
    const Color selection;
    const Color focusRing;
    const Color danger;
    const Color warning;
    const Color success;

    friend bool operator==(const SemanticColors &, const SemanticColors &) = default;
};

struct Materials final {
    const Material panel;
    const Material launcher;
    const Material notification;
    const Material menu;

    friend bool operator==(const Materials &, const Materials &) = default;
};

struct Border final {
    const double thicknessPx;
    const double minimumContrastRatio;

    friend bool operator==(const Border &, const Border &) = default;
};

struct Shadow final {
    const double offsetYPx;
    const double blurRadiusPx;
    const double opacity;

    friend bool operator==(const Shadow &, const Shadow &) = default;
};

struct Typography final {
    const std::string bodyFamily;
    const double bodySizePx;
    const double titleSizePx;
    const std::uint16_t weightNormal;
    const std::uint16_t weightEmphasis;

    friend bool operator==(const Typography &, const Typography &) = default;
};

struct Motion final {
    const std::uint16_t fastMs;
    const std::uint16_t normalMs;
    const MotionEasing easing;

    friend bool operator==(const Motion &, const Motion &) = default;
};

struct SemanticTokens final {
    const SemanticColors colors;
    const Materials materials;
    const Border border;
    const Shadow shadow;
    const Typography typography;
    const NumberMap spacing;
    const NumberMap panel;
    const NumberMap decoration;
    const NumberMap icon;
    const NumberMap focus;
    const Motion motion;
    const NumberMap targets;

    friend bool operator==(const SemanticTokens &, const SemanticTokens &) = default;
};

struct ComponentStyle final {
    const double radiusPx;
    const double paddingPx;
    const double borderWidthPx;

    friend bool operator==(const ComponentStyle &, const ComponentStyle &) = default;
};

struct ComponentTokens final {
    const ComponentStyle taskButton;
    const ComponentStyle launcherTile;
    const ComponentStyle titlebarButton;
    const ComponentStyle notificationCard;
    const ComponentStyle quickSetting;
    const ComponentStyle tooltip;
    const ComponentStyle menuItem;

    friend bool operator==(const ComponentTokens &, const ComponentTokens &) = default;
};

struct ReducedMotionOverride final {
    const double durationScale;

    friend bool operator==(const ReducedMotionOverride &, const ReducedMotionOverride &) = default;
};

struct TransparencyOverride final {
    const bool forceOpaque;

    friend bool operator==(const TransparencyOverride &, const TransparencyOverride &) = default;
};

struct HighContrastOverride final {
    const double minimumContrastRatio;
    const double focusWidthPx;

    friend bool operator==(const HighContrastOverride &, const HighContrastOverride &) = default;
};

struct AccessibilityOverrides final {
    const ReducedMotionOverride reducedMotion;
    const TransparencyOverride transparencyDisabled;
    const HighContrastOverride highContrast;
    const double minimumTargetSizePx;

    friend bool operator==(const AccessibilityOverrides &,
                           const AccessibilityOverrides &) = default;
};

struct CapabilityFallbacks final {
    const std::string blur;
    const std::string thumbnails;

    friend bool operator==(const CapabilityFallbacks &, const CapabilityFallbacks &) = default;
};

/// One complete, validated, immutable version-1 source document. Resolution is separate.
struct ThemeDocument final {
    const std::uint32_t schemaVersion;
    const Layer layer;
    const std::optional<Profile> profile;
    const std::optional<std::string> profileDisplayName;
    const PrimitiveTokens primitive;
    const SemanticTokens semantic;
    const ComponentTokens component;
    const AccessibilityOverrides accessibilityOverrides;
    const CapabilityFallbacks capabilityFallbacks;

    friend bool operator==(const ThemeDocument &, const ThemeDocument &) = default;
};

} // namespace prismdrake::theme
