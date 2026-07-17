#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prismdrake::config {

enum class Profile : std::uint8_t { lustre, forge };
enum class BlurQuality : std::uint8_t { off, low, balanced, high };
enum class PanelEdge : std::uint8_t { top, right, bottom, left };
enum class AutohideMode : std::uint8_t { never, always, overlap };
enum class GroupingMode : std::uint8_t { always, when_full, never };
enum class ClockFormat : std::uint8_t { locale, hour_12, hour_24 };
enum class LauncherLayout : std::uint8_t { compact, expanded };
enum class SearchProvider : std::uint8_t { applications, settings, recent_files };
enum class RecentItemsPolicy : std::uint8_t { disabled, local_only };
enum class NotificationHistory : std::uint8_t { disabled, session, persistent };
enum class WallpaperMode : std::uint8_t { solid, center, fit, fill, stretch, tile };
enum class FocusEmphasis : std::uint8_t { standard, strong };

/// Immutable, normalized appearance values from configuration version 1.
struct Appearance final {
    const std::string accent;
    const bool transparencyEnabled;
    const BlurQuality blurQuality;
    const bool reducedMotion;
    const bool highContrast;
    const double textScale;
    const std::string cursorTheme;
    const std::uint16_t cursorSizePx;
    const std::string iconTheme;

    friend bool operator==(const Appearance &, const Appearance &) = default;
};

struct Panel final {
    const PanelEdge edge;
    const std::uint16_t sizePx;
    const AutohideMode autohide;
    const GroupingMode grouping;
    const ClockFormat clockFormat;
    const std::vector<std::string> outputs;

    friend bool operator==(const Panel &, const Panel &) = default;
};

struct Launcher final {
    const LauncherLayout layout;
    const std::vector<SearchProvider> searchProviders;
    const RecentItemsPolicy recentItems;

    friend bool operator==(const Launcher &, const Launcher &) = default;
};

struct Notifications final {
    const bool enabled;
    const NotificationHistory history;
    const std::uint32_t defaultTimeoutMs;
    const bool doNotDisturb;

    friend bool operator==(const Notifications &, const Notifications &) = default;
};

struct Desktop final {
    const WallpaperMode wallpaperMode;
    const bool iconsEnabled;

    friend bool operator==(const Desktop &, const Desktop &) = default;
};

struct Integration final {
    const bool exportGtk;
    const bool exportQt;
    const bool exportXsettings;
    const bool exportPortal;

    friend bool operator==(const Integration &, const Integration &) = default;
};

/// Accessibility values remain independent from profile identity and defaults.
struct Accessibility final {
    const FocusEmphasis focusEmphasis;
    const double animationScale;
    const std::uint16_t minimumTargetSizePx;

    friend bool operator==(const Accessibility &, const Accessibility &) = default;
};

struct Keyboard final {
    const bool menuKeyOpensLauncher;
    const bool focusWraps;

    friend bool operator==(const Keyboard &, const Keyboard &) = default;
};

/// Developer settings after production-policy normalization.
struct Developer final {
    const bool diagnosticsEnabled;
    const std::vector<std::string> mockCapabilityOverrides;

    friend bool operator==(const Developer &, const Developer &) = default;
};

/// Complete immutable normalized configuration. Partial configurations are never returned.
struct Configuration final {
    const std::uint32_t schemaVersion;
    const Profile profile;
    const Appearance appearance;
    const Panel panel;
    const Launcher launcher;
    const Notifications notifications;
    const Desktop desktop;
    const Integration integration;
    const Accessibility accessibility;
    const Keyboard keyboard;
    const Developer developer;

    friend bool operator==(const Configuration &, const Configuration &) = default;
};

} // namespace prismdrake::config
