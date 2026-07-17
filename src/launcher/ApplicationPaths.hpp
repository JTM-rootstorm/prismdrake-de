#pragma once

#include "Result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumApplicationPathValueBytes = 32U * 1024U;
inline constexpr std::size_t maximumApplicationPathEnvironmentBytes = 64U * 1024U;
inline constexpr std::size_t maximumApplicationDataDirectoryEntries = 128U;

/// Environment values used to resolve standard application discovery roots.
struct ApplicationPathEnvironment final {
    std::optional<std::string> home;
    std::optional<std::string> dataHome;
    std::optional<std::string> dataDirectories;
};

/// Ordered, deduplicated application directories in XDG lookup precedence.
struct ApplicationPaths final {
    std::vector<std::filesystem::path> applicationDirectories;

    friend bool operator==(const ApplicationPaths &, const ApplicationPaths &) = default;
};

/// Resolves bounded XDG application directories without accessing the filesystem.
///
/// Invalid relative and empty XDG_DATA_DIRS components are ignored as required
/// by the XDG Base Directory Specification. The complete supplied environment
/// envelope is byte-bounded and rejects embedded nulls before selecting which
/// values are needed. Symlink resolution and directory scanning belong to later
/// launcher discovery layers.
[[nodiscard]] foundation::Result<ApplicationPaths>
resolveApplicationPaths(const ApplicationPathEnvironment &environment);

/// Captures only the process environment values used by resolveApplicationPaths().
[[nodiscard]] ApplicationPathEnvironment currentProcessApplicationPathEnvironment();

/// Resolves application directories from the current process environment.
[[nodiscard]] foundation::Result<ApplicationPaths> resolveCurrentProcessApplicationPaths();

} // namespace prismdrake::launcher
