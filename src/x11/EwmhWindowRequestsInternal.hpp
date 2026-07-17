#pragma once

#include "EwmhWindowRequests.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace prismdrake::x11::detail {

inline constexpr std::uint8_t clientMessageResponseType = 33U;
inline constexpr std::uint8_t clientMessageFormat = 32U;
inline constexpr std::uint32_t pagerSourceIndication = 2U;
inline constexpr std::uint32_t iconicState = 3U;
inline constexpr std::uint32_t substructureNotifyMask = 1U << 19U;
inline constexpr std::uint32_t substructureRedirectMask = 1U << 20U;
inline constexpr std::uint32_t ewmhRequestEventMask =
    substructureNotifyMask | substructureRedirectMask;

/// Complete copied fields for one root-directed EWMH ClientMessage.
struct EwmhClientMessageFields final {
    std::uint8_t responseType;
    std::uint8_t format;
    std::uint16_t sequence;
    WindowId target;
    AtomId messageType;
    std::array<std::uint32_t, 5> data;
    std::uint32_t destinationMask;

    friend bool operator==(const EwmhClientMessageFields &,
                           const EwmhClientMessageFields &) = default;
};

[[nodiscard]] EwmhClientMessageFields
encodeActivateRequest(WindowId target, AtomId activeWindowAtom, std::uint32_t userActionTimestamp,
                      std::optional<WindowId> currentlyActive) noexcept;
[[nodiscard]] EwmhClientMessageFields
encodeCloseRequest(WindowId target, AtomId closeWindowAtom,
                   std::uint32_t userActionTimestamp) noexcept;
[[nodiscard]] EwmhClientMessageFields encodeMinimizeRequest(WindowId target,
                                                            AtomId wmChangeStateAtom) noexcept;

} // namespace prismdrake::x11::detail
