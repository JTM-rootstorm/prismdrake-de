#pragma once

#include "Result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace prismdrake::foundation {

/// Environment values used to resolve Prismdrake's XDG directories.
///
/// Keeping this as a value type allows callers and tests to resolve paths
/// without mutating or depending on the current process environment.
struct XdgEnvironment {
    std::optional<std::string> home;
    std::optional<std::string> configHome;
    std::optional<std::string> dataHome;
    std::optional<std::string> stateHome;
    std::optional<std::string> cacheHome;
    std::optional<std::string> runtimeDirectory;
};

/// Absolute Prismdrake-owned subdirectories resolved from the XDG environment.
struct XdgPaths {
    std::filesystem::path configDirectory;
    std::filesystem::path dataDirectory;
    std::filesystem::path stateDirectory;
    std::filesystem::path cacheDirectory;
    std::filesystem::path runtimeDirectory;

    friend bool operator==(const XdgPaths &, const XdgPaths &) = default;
};

/// Resolves absolute Prismdrake XDG paths without accessing the filesystem.
[[nodiscard]] Result<XdgPaths> resolveXdgPaths(const XdgEnvironment &environment);

/// Captures only the environment variables needed by resolveXdgPaths().
[[nodiscard]] XdgEnvironment currentProcessXdgEnvironment();

/// Resolves XDG paths from the current process environment.
[[nodiscard]] Result<XdgPaths> resolveCurrentProcessXdgPaths();

/// Validates the existing XDG runtime base and Prismdrake subdirectory without
/// creating, following, or modifying either path.
[[nodiscard]] Result<void>
validateRuntimeDirectoryBoundary(const std::filesystem::path &runtimeBaseDirectory,
                                 const std::filesystem::path &prismdrakeRuntimeDirectory,
                                 std::uintmax_t expectedUserId);

/// Returns the effective user ID used for current-process filesystem access.
[[nodiscard]] std::uintmax_t currentProcessUserId() noexcept;

/// Validates resolved runtime paths for the current process user.
[[nodiscard]] Result<void> validateCurrentProcessRuntimeDirectory(const XdgPaths &paths);

} // namespace prismdrake::foundation
