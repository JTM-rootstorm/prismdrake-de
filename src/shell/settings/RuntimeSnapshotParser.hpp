#pragma once

#include "Result.hpp"
#include "SettingsSnapshot.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace prismdrake::shell::settings {

/// Parses one complete canonical Experimental runtime snapshot reply.
///
/// The outer D-Bus generation must match the embedded generation. Parsing is allocation-bounded,
/// rejects unknown or partial version-1 content, reconstructs immutable typed settings, and
/// verifies the exact canonical bytes through the authoritative serializer before publication.
[[nodiscard]] foundation::Result<std::shared_ptr<const prismdrake::settings::SettingsSnapshot>>
parseRuntimeSnapshot(std::uint64_t outerGeneration, std::string_view json);

} // namespace prismdrake::shell::settings
