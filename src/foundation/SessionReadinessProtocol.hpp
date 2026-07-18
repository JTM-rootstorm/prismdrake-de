#pragma once

#include <cstdint>
#include <string_view>

namespace prismdrake::foundation {

/// Private, per-launch descriptor used for the session-to-shell readiness handshake.
inline constexpr std::string_view sessionReadinessDescriptorEnvironment =
    "PRISMDRAKE_SESSION_READY_FD";

/// One complete event value means that the initial panel epoch exists.
inline constexpr std::uint64_t sessionReadinessMessage = 0x505249534D524459ULL;

} // namespace prismdrake::foundation
