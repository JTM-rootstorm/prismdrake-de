#pragma once

#include "PanelStrutGeometry.hpp"
#include "Result.hpp"
#include "X11Types.hpp"

#include <cstdint>

namespace prismdrake::x11 {

class AtomCache;
class X11Connection;

/// Checked standards-only publication of one bottom panel's EWMH properties.
///
/// The adapter owns no window or geometry policy. It derives exact strut
/// values through PanelStrutGeometry and never retains connection, atom, or
/// window identifiers after a call returns.
class DockProperties final {
  public:
    /// Derives and publishes the dock type and both strut properties through
    /// checked writes. Invalid geometry clears any prior dock publication;
    /// partial write failure triggers best-effort cleanup before returning.
    [[nodiscard]] static foundation::Result<BottomPanelStrut>
    publishBottomPanel(X11Connection &connection, const AtomCache &atoms, WindowId window,
                       RootGeometry root, OutputGeometry output, std::uint32_t panelHeight);

    /// Removes the dock type and both reservations. A window destroyed before
    /// or during removal is already clean and is treated as success.
    [[nodiscard]] static foundation::Result<void> remove(X11Connection &connection,
                                                         const AtomCache &atoms, WindowId window);
};

} // namespace prismdrake::x11
