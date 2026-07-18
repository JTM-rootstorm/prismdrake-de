#pragma once

#include "SdBusApi.hpp"

#include <cstdint>
#include <optional>

namespace prismdrake::ipc {

using EffectiveUserId = std::uintmax_t;

/// Returns whether a resolved peer effective user matches the service process.
///
/// Missing credentials fail closed. This overload keeps the policy independently
/// testable without exposing provider-specific credential handles to callers.
[[nodiscard]] bool isSameEffectiveUser(std::optional<EffectiveUserId> peerEffectiveUserId,
                                       EffectiveUserId processEffectiveUserId) noexcept;

/// Resolves an sd-bus sender's effective user and compares it with this process.
///
/// Null messages, failed queries, incomplete credential replies, and mismatched
/// effective user IDs all fail closed.
[[nodiscard]] bool isSameUserPeer(sd_bus_message *message) noexcept;

} // namespace prismdrake::ipc
