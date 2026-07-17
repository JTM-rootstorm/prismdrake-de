#pragma once

#include "Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace prismdrake::session {

inline constexpr std::size_t maximumSettingsReadinessSnapshotBytes = 1024U * 1024U;
inline constexpr auto maximumSettingsReadinessTimeout = std::chrono::seconds{10};

struct SettingsReadiness final {
    std::uint64_t generation;
    std::size_t snapshotBytes;
};

/// Performs one bounded Experimental SettingsSnapshot1 request on the inherited
/// user session bus. The payload is size-checked but intentionally not retained;
/// consumers fetch their own complete immutable copy after they start.
[[nodiscard]] foundation::Result<SettingsReadiness>
probeSettingsReadiness(std::chrono::milliseconds timeout);

} // namespace prismdrake::session
