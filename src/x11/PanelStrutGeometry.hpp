#pragma once

#include "OutputTopology.hpp"
#include "Result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace prismdrake::x11 {

/// Derived bottom-panel rectangle expressed in root coordinates.
struct BottomPanelGeometry final {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t width;
    std::uint32_t height;

    friend bool operator==(const BottomPanelGeometry &, const BottomPanelGeometry &) = default;
};

/// Exact CARDINAL arrays for _NET_WM_STRUT and _NET_WM_STRUT_PARTIAL.
struct BottomPanelStrut final {
    BottomPanelGeometry panel;
    std::array<std::uint32_t, 4U> strut;
    std::array<std::uint32_t, 12U> strutPartial;

    friend bool operator==(const BottomPanelStrut &, const BottomPanelStrut &) = default;
};

struct BottomPanelStrutRequest final {
    OutputGeometry output;
    std::uint32_t panelHeight;
};

/// Validates containment and derives one full-output-width bottom panel plus
/// its EWMH reservation arrays. This pure calculation owns no WM or X11 state.
[[nodiscard]] foundation::Result<BottomPanelStrut>
calculateBottomPanelStrut(RootGeometry root, OutputGeometry output, std::uint32_t panelHeight);

/// Derives one independent reservation per requested output. The complete set
/// is rejected if any panel is invalid; each result retains only its own edge
/// interval and never combines another panel's reservation.
[[nodiscard]] foundation::Result<std::vector<BottomPanelStrut>>
calculateBottomPanelStruts(RootGeometry root, std::span<const BottomPanelStrutRequest> requests);

} // namespace prismdrake::x11
