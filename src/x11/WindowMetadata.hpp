#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::x11 {

inline constexpr std::size_t maximumWindowTitleBytes = 1024U;
inline constexpr std::size_t maximumWindowTitleCodePoints = 256U;
inline constexpr std::size_t maximumWmClassBytes = 512U;
inline constexpr std::size_t maximumWmClassPartBytes = 255U;
inline constexpr std::size_t maximumWindowTypes = 16U;
inline constexpr std::size_t maximumWindowStates = 32U;
inline constexpr std::size_t maximumNetWmIconBytes = 1024U * 1024U;
inline constexpr std::size_t maximumWindowIcons = 32U;
inline constexpr std::uint32_t maximumWindowIconDimension = 512U;
inline constexpr std::size_t maximumWindowIconPixels = 512U * 512U;
inline constexpr std::uint32_t allWorkspaces = 0xffffffffU;
inline constexpr std::uint32_t wmHintsUrgencyFlag = 1U << 8U;

enum class WindowType : std::uint8_t {
    normal,
    dialog,
    utility,
    toolbar,
    menu,
    splash,
    dropdownMenu,
    popupMenu,
    tooltip,
    notification,
    combo,
    dragAndDrop,
    dock,
    desktop,
};

enum class WindowState : std::uint8_t {
    hidden,
    fullscreen,
    modal,
    skipTaskbar,
    demandsAttention,
};

enum class ApplicationIdentitySource : std::uint8_t {
    wmClass,
    genericUnknown,
};

struct ApplicationIdentityEvidence final {
    ApplicationIdentitySource source;
    std::optional<std::string> instance;
    std::optional<std::string> className;
    std::string groupingKey;

    friend bool operator==(const ApplicationIdentityEvidence &,
                           const ApplicationIdentityEvidence &) = default;
};

struct WindowIcon final {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint32_t> argbPixels;

    friend bool operator==(const WindowIcon &, const WindowIcon &) = default;
};

/// Protocol-neutral, already type-checked property payloads for one live window.
/// Unknown EWMH type/state atoms are omitted by the property adapter; recognized
/// values remain ordered exactly as advertised by the client.
struct WindowMetadataObservation final {
    WindowId window;
    std::optional<std::string> utf8Title;
    std::optional<std::string> legacyTitle;
    std::optional<std::vector<std::uint8_t>> wmClass;
    std::vector<WindowType> windowTypes;
    std::vector<WindowState> windowStates;
    std::optional<std::uint32_t> workspace;
    std::optional<std::uint32_t> wmHintsFlags;
    std::optional<WindowId> transientFor;
    std::optional<std::vector<std::uint32_t>> netWmIcon;
};

/// One bounded all-or-nothing metadata snapshot. No property content is ever
/// included in validation diagnostics.
struct WindowMetadata final {
    WindowId window;
    std::string displayTitle;
    ApplicationIdentityEvidence identity;
    WindowType type;
    bool typeWasExplicit;
    std::vector<WindowState> states;
    std::optional<std::uint32_t> workspace;
    bool onAllWorkspaces;
    bool minimized;
    bool urgent;
    bool skipTaskbar;
    bool modal;
    std::optional<WindowId> transientFor;
    std::vector<WindowIcon> icons;
};

/// Validates and decodes one complete observation without publishing partial data.
[[nodiscard]] foundation::Result<WindowMetadata>
decodeWindowMetadata(const WindowMetadataObservation &observation);

} // namespace prismdrake::x11
