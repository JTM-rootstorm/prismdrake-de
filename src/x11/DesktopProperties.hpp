#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

namespace prismdrake::x11 {

class AtomCache;
class X11Connection;

/// Checked standards-only publication of the EWMH desktop window type.
///
/// This adapter publishes no stacking request. The active window manager
/// remains authoritative for placing desktop windows below ordinary clients.
class DesktopProperties final {
  public:
    [[nodiscard]] static foundation::Result<void> publish(X11Connection &connection,
                                                          const AtomCache &atoms, WindowId window);

    /// Removes only the desktop type property. A target destroyed during
    /// cleanup is already clean and is treated as success.
    [[nodiscard]] static foundation::Result<void> remove(X11Connection &connection,
                                                         const AtomCache &atoms, WindowId window);
};

} // namespace prismdrake::x11
