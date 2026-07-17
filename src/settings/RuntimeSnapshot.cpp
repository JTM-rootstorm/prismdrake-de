#include "RuntimeSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::settings {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] std::string colorString(theme::Color color) {
    std::array<char, 10U> buffer{};
    (void)std::snprintf(buffer.data(), buffer.size(), "#%08X", color.rgba);
    return buffer.data();
}

template <typename Value>
[[nodiscard]] Json stringMap(const std::map<std::string, Value, std::less<>> &values) {
    Json object = Json::object();
    for (const auto &[key, value] : values) {
        object[key] = value;
    }
    return object;
}

[[nodiscard]] Json colorMap(const theme::ColorMap &values) {
    Json object = Json::object();
    for (const auto &[key, value] : values) {
        object[key] = colorString(value);
    }
    return object;
}

template <typename Enum, typename Value, std::size_t Size>
[[nodiscard]] std::string_view enumId(Enum value, const std::array<Value, Size> &ids) {
    return ids.at(static_cast<std::size_t>(value));
}

[[nodiscard]] std::string_view blurQualityId(config::BlurQuality value) {
    constexpr std::array ids{"off", "low", "balanced", "high"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view panelEdgeId(config::PanelEdge value) {
    constexpr std::array ids{"top", "right", "bottom", "left"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view autohideId(config::AutohideMode value) {
    constexpr std::array ids{"never", "always", "overlap"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view groupingId(config::GroupingMode value) {
    constexpr std::array ids{"always", "when_full", "never"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view clockFormatId(config::ClockFormat value) {
    constexpr std::array ids{"locale", "12_hour", "24_hour"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view launcherLayoutId(config::LauncherLayout value) {
    constexpr std::array ids{"compact", "expanded"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view searchProviderId(config::SearchProvider value) {
    constexpr std::array ids{"applications", "settings", "recent_files"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view recentItemsId(config::RecentItemsPolicy value) {
    constexpr std::array ids{"disabled", "local_only"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view notificationHistoryId(config::NotificationHistory value) {
    constexpr std::array ids{"disabled", "session", "persistent"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view wallpaperModeId(config::WallpaperMode value) {
    constexpr std::array ids{"solid", "center", "fit", "fill", "stretch", "tile"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view focusEmphasisId(config::FocusEmphasis value) {
    constexpr std::array ids{"standard", "strong"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view motionEasingId(theme::MotionEasing value) {
    constexpr std::array ids{"linear", "ease_out", "ease_in_out", "crisp"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view thumbnailPresentationId(theme::ThumbnailPresentation value) {
    constexpr std::array ids{"provided_thumbnail", "application_icon_title_state"};
    return enumId(value, ids);
}

[[nodiscard]] std::string_view themeSourceId(theme::ThemeSource value) {
    constexpr std::array ids{"packaged_base", "packaged_lustre", "packaged_forge",
                             "packaged_accessibility"};
    return enumId(value, ids);
}

[[nodiscard]] Json configurationJson(const config::Configuration &configuration) {
    Json providers = Json::array();
    for (const auto provider : configuration.launcher.searchProviders) {
        providers.push_back(searchProviderId(provider));
    }
    return {{"schema_version", configuration.schemaVersion},
            {"appearance",
             {{"accent", configuration.appearance.accent},
              {"transparency_enabled", configuration.appearance.transparencyEnabled},
              {"blur_quality", blurQualityId(configuration.appearance.blurQuality)},
              {"reduced_motion", configuration.appearance.reducedMotion},
              {"high_contrast", configuration.appearance.highContrast},
              {"text_scale", configuration.appearance.textScale},
              {"cursor_theme", configuration.appearance.cursorTheme},
              {"cursor_size_px", configuration.appearance.cursorSizePx},
              {"icon_theme", configuration.appearance.iconTheme}}},
            {"panel",
             {{"edge", panelEdgeId(configuration.panel.edge)},
              {"size_px", configuration.panel.sizePx},
              {"autohide", autohideId(configuration.panel.autohide)},
              {"grouping", groupingId(configuration.panel.grouping)},
              {"clock_format", clockFormatId(configuration.panel.clockFormat)},
              {"outputs", configuration.panel.outputs}}},
            {"launcher",
             {{"layout", launcherLayoutId(configuration.launcher.layout)},
              {"search_providers", std::move(providers)},
              {"recent_items", recentItemsId(configuration.launcher.recentItems)}}},
            {"notifications",
             {{"enabled", configuration.notifications.enabled},
              {"history", notificationHistoryId(configuration.notifications.history)},
              {"default_timeout_ms", configuration.notifications.defaultTimeoutMs},
              {"do_not_disturb", configuration.notifications.doNotDisturb}}},
            {"desktop",
             {{"wallpaper_mode", wallpaperModeId(configuration.desktop.wallpaperMode)},
              {"icons_enabled", configuration.desktop.iconsEnabled}}},
            {"integration",
             {{"export_gtk", configuration.integration.exportGtk},
              {"export_qt", configuration.integration.exportQt},
              {"export_xsettings", configuration.integration.exportXsettings},
              {"export_portal", configuration.integration.exportPortal}}},
            {"accessibility",
             {{"focus_emphasis", focusEmphasisId(configuration.accessibility.focusEmphasis)},
              {"animation_scale", configuration.accessibility.animationScale},
              {"minimum_target_size_px", configuration.accessibility.minimumTargetSizePx}}},
            {"keyboard",
             {{"menu_key_opens_launcher", configuration.keyboard.menuKeyOpensLauncher},
              {"focus_wraps", configuration.keyboard.focusWraps}}},
            {"developer",
             {{"diagnostics_enabled", configuration.developer.diagnosticsEnabled},
              {"mock_capability_overrides", configuration.developer.mockCapabilityOverrides}}}};
}

[[nodiscard]] Json authoredMaterialJson(const theme::Material &material) {
    const auto fallbackKind =
        material.fallback.kind == theme::FallbackKind::alpha ? "alpha" : "opaque";
    return {{"tint", colorString(material.tint)},
            {"opacity", material.opacity},
            {"blur_request",
             {{"enabled", material.blurRequest.enabled},
              {"radius_px", material.blurRequest.radiusPx},
              {"saturation", material.blurRequest.saturation}}},
            {"fallback",
             {{"kind", fallbackKind},
              {"color", colorString(material.fallback.color)},
              {"opacity", material.fallback.opacity}}}};
}

[[nodiscard]] Json resolvedMaterialJson(const theme::ResolvedMaterial &material) {
    return {{"color", colorString(material.color)},     {"opacity", material.opacity},
            {"blur_requested", material.blurRequested}, {"blur_radius_px", material.blurRadiusPx},
            {"saturation", material.saturation},        {"used_fallback", material.usedFallback}};
}

[[nodiscard]] Json componentStyleJson(const theme::ComponentStyle &style) {
    return {{"radius_px", style.radiusPx},
            {"padding_px", style.paddingPx},
            {"border_width_px", style.borderWidthPx}};
}

[[nodiscard]] Json themeJson(const theme::ResolvedThemeCandidate &themeCandidate) {
    Json sources = Json::array();
    for (const auto source : themeCandidate.sources) {
        sources.push_back(themeSourceId(source));
    }
    Json warnings = Json::array();
    for (const auto warning : themeCandidate.warnings) {
        warnings.push_back(themeWarningId(warning));
    }
    const auto &semantic = themeCandidate.semantic;
    const auto &components = themeCandidate.component;
    return {{"schema_version", themeCandidate.schemaVersion},
            {"profile_id", themeCandidate.profile == theme::Profile::lustre ? "lustre" : "forge"},
            {"profile_display_name", themeCandidate.profileDisplayName},
            {"logical_source_ids", std::move(sources)},
            {"primitive",
             {{"colors", colorMap(themeCandidate.primitive.colors)},
              {"spacing_px", stringMap(themeCandidate.primitive.spacingPx)},
              {"font_families", themeCandidate.primitive.fontFamilies},
              {"font_size_px", stringMap(themeCandidate.primitive.fontSizePx)},
              {"duration_ms", stringMap(themeCandidate.primitive.durationMs)},
              {"radius_px", stringMap(themeCandidate.primitive.radiusPx)},
              {"opacity", stringMap(themeCandidate.primitive.opacity)}}},
            {"semantic",
             {{"colors",
               {{"panel_surface", colorString(semantic.colors.panelSurface)},
                {"elevated_surface", colorString(semantic.colors.elevatedSurface)},
                {"window_frame", colorString(semantic.colors.windowFrame)},
                {"border_active", colorString(semantic.colors.borderActive)},
                {"border_inactive", colorString(semantic.colors.borderInactive)},
                {"text_primary", colorString(semantic.colors.textPrimary)},
                {"text_muted", colorString(semantic.colors.textMuted)},
                {"selection", colorString(semantic.colors.selection)},
                {"focus_ring", colorString(semantic.colors.focusRing)},
                {"danger", colorString(semantic.colors.danger)},
                {"warning", colorString(semantic.colors.warning)},
                {"success", colorString(semantic.colors.success)}}},
              {"materials",
               {{"panel", authoredMaterialJson(semantic.materials.panel)},
                {"launcher", authoredMaterialJson(semantic.materials.launcher)},
                {"notification", authoredMaterialJson(semantic.materials.notification)},
                {"menu", authoredMaterialJson(semantic.materials.menu)}}},
              {"border",
               {{"thickness_px", semantic.border.thicknessPx},
                {"minimum_contrast_ratio", semantic.border.minimumContrastRatio}}},
              {"shadow",
               {{"offset_y_px", semantic.shadow.offsetYPx},
                {"blur_radius_px", semantic.shadow.blurRadiusPx},
                {"opacity", semantic.shadow.opacity}}},
              {"typography",
               {{"body_family", semantic.typography.bodyFamily},
                {"body_size_px", semantic.typography.bodySizePx},
                {"title_size_px", semantic.typography.titleSizePx},
                {"weight_normal", semantic.typography.weightNormal},
                {"weight_emphasis", semantic.typography.weightEmphasis}}},
              {"spacing", stringMap(semantic.spacing)},
              {"panel", stringMap(semantic.panel)},
              {"decoration", stringMap(semantic.decoration)},
              {"icon", stringMap(semantic.icon)},
              {"focus", stringMap(semantic.focus)},
              {"motion",
               {{"fast_ms", semantic.motion.fastMs},
                {"normal_ms", semantic.motion.normalMs},
                {"easing", motionEasingId(semantic.motion.easing)}}},
              {"targets", stringMap(semantic.targets)}}},
            {"component",
             {{"task_button", componentStyleJson(components.taskButton)},
              {"launcher_tile", componentStyleJson(components.launcherTile)},
              {"titlebar_button", componentStyleJson(components.titlebarButton)},
              {"notification_card", componentStyleJson(components.notificationCard)},
              {"quick_setting", componentStyleJson(components.quickSetting)},
              {"tooltip", componentStyleJson(components.tooltip)},
              {"menu_item", componentStyleJson(components.menuItem)}}},
            {"effective_accessibility",
             {{"high_contrast", themeCandidate.accessibility.highContrast},
              {"reduced_motion", themeCandidate.accessibility.reducedMotion},
              {"transparency_disabled", themeCandidate.accessibility.transparencyDisabled},
              {"text_scale", themeCandidate.accessibility.textScale},
              {"animation_scale", themeCandidate.accessibility.animationScale},
              {"focus_width_px", themeCandidate.accessibility.focusWidthPx},
              {"minimum_target_size_px", themeCandidate.accessibility.minimumTargetSizePx},
              {"minimum_contrast_ratio", themeCandidate.accessibility.minimumContrastRatio}}},
            {"capability_fallbacks",
             {{"blur", themeCandidate.capabilityFallbacks.blur},
              {"thumbnails", themeCandidate.capabilityFallbacks.thumbnails}}},
            {"resolved_materials",
             {{"panel", resolvedMaterialJson(themeCandidate.materials.panel)},
              {"launcher", resolvedMaterialJson(themeCandidate.materials.launcher)},
              {"notification", resolvedMaterialJson(themeCandidate.materials.notification)},
              {"menu", resolvedMaterialJson(themeCandidate.materials.menu)}}},
            {"thumbnail_presentation", thumbnailPresentationId(themeCandidate.thumbnails)},
            {"warnings", std::move(warnings)}};
}

} // namespace

foundation::Result<SerializedRuntimeSnapshot>
serializeRuntimeSnapshot(foundation::Generation generation, const SettingsCandidate &candidate) {
    try {
        Json warningIds = Json::array();
        for (const auto warning : candidate.warnings) {
            warningIds.push_back(settingsWarningId(warning));
        }
        const Json document = {
            {"schema_version", runtimeSnapshotSchemaVersion},
            {"generation", generation.value()},
            {"profile_id", profileId(candidate.configuration.profile)},
            {"configuration_source_id",
             configurationSourceId(candidate.provenance.configurationSource)},
            {"runtime_profile_override", candidate.provenance.runtimeProfileOverride},
            {"settings", configurationJson(candidate.configuration)},
            {"theme", themeJson(candidate.theme)},
            {"validation_warning_ids", std::move(warningIds)},
            {"restart_required_domains", Json::array()},
        };
        auto bytes = document.dump();
        if (bytes.size() > maximumRuntimeSnapshotBytes) {
            return foundation::Result<SerializedRuntimeSnapshot>::failure(
                {foundation::ErrorCode::too_large,
                 "The complete runtime snapshot exceeds the 1 MiB transport limit.",
                 "Reduce bounded theme token data before publication."});
        }
        return foundation::Result<SerializedRuntimeSnapshot>::success(
            SerializedRuntimeSnapshot{generation.value(), std::move(bytes)});
    } catch (const std::exception &) {
        return foundation::Result<SerializedRuntimeSnapshot>::failure(
            {foundation::ErrorCode::validation_error,
             "The complete runtime snapshot could not be serialized safely.",
             "Review the bounded normalized settings and theme candidate."});
    }
}

} // namespace prismdrake::settings
