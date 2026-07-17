#pragma once

#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace prismdrake::x11 {

class X11Connection;

inline constexpr std::size_t maximumEwmhSupportedAtoms = 256U;

/// Verification state for the root EWMH owner handshake.
///
/// Unavailable and malformed states are reduced-capability outcomes, not fatal
/// X11 transport failures.
enum class EwmhDiscoveryStatus : std::uint8_t {
    unavailable,
    malformed,
    verified,
};

/// Returns one fixed lower-snake-case diagnostic identifier without WM data.
[[nodiscard]] std::string_view ewmhDiscoveryStatusId(EwmhDiscoveryStatus status) noexcept;

/// Standards-only EWMH features required by the bounded PD1 shell proof.
struct EwmhCapabilityFlags final {
    bool clientList = false;
    bool activeWindow = false;
    bool basicWorkspaces = false;
    bool dockWindowType = false;
    bool dockStrutPartial = false;

    friend bool operator==(const EwmhCapabilityFlags &, const EwmhCapabilityFlags &) = default;
};

/// One redacted EWMH discovery result. No WM identity or native Glasswyrm
/// capability is inferred or exposed.
struct EwmhCapabilities final {
    EwmhDiscoveryStatus status = EwmhDiscoveryStatus::unavailable;
    EwmhCapabilityFlags flags;

    friend bool operator==(const EwmhCapabilities &, const EwmhCapabilities &) = default;
};

/// Verifies `_NET_SUPPORTING_WM_CHECK` on the root and supporting window, then
/// reads one bounded `_NET_SUPPORTED` atom set. Absent or malformed EWMH data
/// returns a successful reduced-capability result. Only an unusable X11
/// transport fails the operation.
[[nodiscard]] foundation::Result<EwmhCapabilities>
discoverEwmhCapabilities(X11Connection &connection);

} // namespace prismdrake::x11
