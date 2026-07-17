#pragma once

#include "Result.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::session {

inline constexpr std::size_t maximumSessionEnvironmentEntries = 4096U;
inline constexpr std::size_t maximumSessionEnvironmentEntryBytes = 32U * 1024U;
inline constexpr std::size_t maximumSessionEnvironmentBytes = 1024U * 1024U;

/// Validated inherited environment with only Prismdrake's three desktop identity
/// values replaced. Values are retained for direct execve use and never rendered
/// into diagnostics.
struct PreparedSessionEnvironment final {
    std::vector<std::string> entries;
    std::string display;
    std::filesystem::path runtimeDirectory;
    std::string sessionBusAddress;
};

/// Validates required development-session values and produces an environment for
/// children. Unrelated entries remain byte-for-byte unchanged and in their
/// inherited order. Duplicate required values and malformed entries are rejected.
[[nodiscard]] foundation::Result<PreparedSessionEnvironment>
prepareSessionEnvironment(std::span<const std::string_view> inherited);

/// Reads the bounded current-process environment and delegates to
/// prepareSessionEnvironment. The returned storage owns every child entry.
[[nodiscard]] foundation::Result<PreparedSessionEnvironment> prepareCurrentSessionEnvironment();

} // namespace prismdrake::session
