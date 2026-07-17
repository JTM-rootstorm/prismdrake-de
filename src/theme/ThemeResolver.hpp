#pragma once

#include "Configuration.hpp"
#include "Result.hpp"
#include "Theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace prismdrake::theme {

enum class ThemeSource : std::uint8_t {
    packaged_base,
    packaged_lustre,
    packaged_forge,
    packaged_accessibility,
};

enum class ThemeWarning : std::uint8_t {
    accent_suppressed_high_contrast,
    blur_fallback_active,
    thumbnail_fallback_active,
    safe_mode_active,
};

enum class ThumbnailPresentation : std::uint8_t {
    provided_thumbnail,
    application_icon_title_state,
};

struct ThemeCapabilities final {
    bool blurAvailable = false;
    bool thumbnailsAvailable = false;
};

struct ThemeResolveOptions final {
    ThemeCapabilities capabilities;
    bool safeMode = false;
};

struct ResolvedMaterial final {
    const Color color;
    const double opacity;
    const bool blurRequested;
    const double blurRadiusPx;
    const double saturation;
    const bool usedFallback;

    friend bool operator==(const ResolvedMaterial &, const ResolvedMaterial &) = default;
};

struct ResolvedMaterials final {
    const ResolvedMaterial panel;
    const ResolvedMaterial launcher;
    const ResolvedMaterial notification;
    const ResolvedMaterial menu;

    friend bool operator==(const ResolvedMaterials &, const ResolvedMaterials &) = default;
};

struct EffectiveAccessibility final {
    const bool highContrast;
    const bool reducedMotion;
    const bool transparencyDisabled;
    const double textScale;
    const double animationScale;
    const double focusWidthPx;
    const double minimumTargetSizePx;
    const double minimumContrastRatio;

    friend bool operator==(const EffectiveAccessibility &,
                           const EffectiveAccessibility &) = default;
};

/// Complete generationless theme candidate for the later combined settings publication.
struct ResolvedThemeCandidate final {
    const std::uint32_t schemaVersion;
    const Profile profile;
    const std::string profileDisplayName;
    const std::vector<ThemeSource> sources;
    const PrimitiveTokens primitive;
    const SemanticTokens semantic;
    const ComponentTokens component;
    const EffectiveAccessibility accessibility;
    const CapabilityFallbacks capabilityFallbacks;
    const ResolvedMaterials materials;
    const ThumbnailPresentation thumbnails;
    const std::vector<ThemeWarning> warnings;

    friend bool operator==(const ResolvedThemeCandidate &,
                           const ResolvedThemeCandidate &) = default;
};

/// Resolves one complete candidate from all packaged layers and normalized configuration.
/// Generation assignment is deliberately deferred to the combined settings/theme publisher.
[[nodiscard]] foundation::Result<ResolvedThemeCandidate>
resolveThemeCandidate(const ThemeDocument &base, const ThemeDocument &lustre,
                      const ThemeDocument &forge, const ThemeDocument &accessibility,
                      const config::Configuration &configuration, ThemeResolveOptions options = {});

} // namespace prismdrake::theme
