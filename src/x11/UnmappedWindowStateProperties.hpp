#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

namespace prismdrake::x11 {

class AtomCache;
class X11Connection;

/// Checked EWMH-state publication for an unmapped managed shell window.
class UnmappedWindowStateProperties final {
  public:
    /// Preserves the bounded existing ATOM state list and adds
    /// `_NET_WM_STATE_SKIP_TASKBAR` exactly once. The target must still be an
    /// unmapped, non-override-redirect window so the window manager observes
    /// the policy on its next management pass. This supports both first
    /// publication and publication after a managed window has been withdrawn.
    [[nodiscard]] static foundation::Result<void>
    publishSkipTaskbar(X11Connection &connection, const AtomCache &atoms, WindowId window);
};

} // namespace prismdrake::x11
