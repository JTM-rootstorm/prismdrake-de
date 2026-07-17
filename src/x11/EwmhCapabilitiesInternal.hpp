#pragma once

#include "EwmhCapabilities.hpp"

namespace prismdrake::x11 {

class X11Connection;

namespace detail {

/// Internal deterministic interleave point used only by X11 integration tests.
using EwmhDiscoveryInterleaveHook = void (*)(void *context);

[[nodiscard]] foundation::Result<EwmhCapabilities>
discoverEwmhCapabilitiesWithInterleave(X11Connection &connection,
                                       EwmhDiscoveryInterleaveHook interleave, void *context);

} // namespace detail
} // namespace prismdrake::x11
