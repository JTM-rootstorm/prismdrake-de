#pragma once

#include "Result.hpp"
#include "SettingsSnapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace prismdrake::settings {

inline constexpr std::size_t maximumRuntimeSnapshotBytes = std::size_t{1024} * 1024U;

struct SerializedRuntimeSnapshot final {
    std::uint64_t generation;
    std::string json;
};

/// Serializes one complete candidate as strict, bounded version-1 JSON before publication.
[[nodiscard]] foundation::Result<SerializedRuntimeSnapshot>
serializeRuntimeSnapshot(foundation::Generation generation, const SettingsCandidate &candidate);

} // namespace prismdrake::settings
