#pragma once

#include "PanelStrutGeometry.hpp"
#include "Result.hpp"
#include "X11Types.hpp"

#include <array>
#include <cstdint>

namespace prismdrake::x11::detail {

/// Protocol-neutral owned values used by the checked XCB publication adapter.
struct DockPropertyPayload final {
    std::array<std::uint32_t, 1U> windowType;
    std::array<std::uint32_t, 4U> strut;
    std::array<std::uint32_t, 12U> strutPartial;

    friend bool operator==(const DockPropertyPayload &, const DockPropertyPayload &) = default;
};

[[nodiscard]] foundation::Result<DockPropertyPayload>
makeDockPropertyPayload(AtomId dockWindowType, const BottomPanelStrut &reservation);

/// Rejects publication to the root while keeping identifiers out of errors.
[[nodiscard]] foundation::Result<void> validateDockTarget(WindowId window, WindowId root);

} // namespace prismdrake::x11::detail
