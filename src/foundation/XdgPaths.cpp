#include "XdgPaths.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::foundation {
namespace {

constexpr std::string_view prismdrakeDirectory = "prismdrake";

[[nodiscard]] bool containsNullByte(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

template <typename T>
[[nodiscard]] Result<T> invalidEnvironment(std::string message, std::string recovery) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_environment, std::move(message), std::move(recovery)});
}

[[nodiscard]] Result<std::filesystem::path> resolveHome(const std::optional<std::string> &home) {
    if (!home || home->empty() || containsNullByte(*home)) {
        return invalidEnvironment<std::filesystem::path>(
            "HOME must be set to a valid absolute path when an XDG default is required.",
            "Set HOME to the absolute home directory for the current user.");
    }

    const std::filesystem::path homePath{*home};
    if (!homePath.is_absolute()) {
        return invalidEnvironment<std::filesystem::path>(
            "HOME must be absolute when an XDG default is required.",
            "Set HOME to the absolute home directory for the current user.");
    }

    return Result<std::filesystem::path>::success(homePath.lexically_normal());
}

struct HomeBasedDirectoryInputs {
    const std::optional<std::string> &configuredValue;
    const std::optional<std::string> &home;
    std::filesystem::path defaultSuffix;
};

[[nodiscard]] Result<std::filesystem::path>
resolveHomeBasedDirectory(const HomeBasedDirectoryInputs &inputs) {
    if (inputs.configuredValue && !inputs.configuredValue->empty()) {
        if (containsNullByte(*inputs.configuredValue)) {
            return invalidEnvironment<std::filesystem::path>(
                "An XDG directory contains an invalid null byte.",
                "Set every XDG directory to a valid absolute path or remove the invalid value.");
        }

        const std::filesystem::path configuredPath{*inputs.configuredValue};
        if (configuredPath.is_absolute()) {
            return Result<std::filesystem::path>::success(
                (configuredPath / prismdrakeDirectory).lexically_normal());
        }
    }

    auto resolvedHome = resolveHome(inputs.home);
    if (!resolvedHome) {
        return Result<std::filesystem::path>::failure(resolvedHome.error());
    }

    return Result<std::filesystem::path>::success(
        (std::move(resolvedHome).value() / inputs.defaultSuffix / prismdrakeDirectory)
            .lexically_normal());
}

[[nodiscard]] Result<std::filesystem::path>
resolveRuntimeDirectory(const std::optional<std::string> &runtimeDirectory) {
    if (!runtimeDirectory || runtimeDirectory->empty()) {
        return invalidEnvironment<std::filesystem::path>(
            "XDG_RUNTIME_DIR is required for Prismdrake runtime state.",
            "Set XDG_RUNTIME_DIR to an absolute per-user runtime directory.");
    }
    if (containsNullByte(*runtimeDirectory)) {
        return invalidEnvironment<std::filesystem::path>(
            "XDG_RUNTIME_DIR contains an invalid null byte.",
            "Set XDG_RUNTIME_DIR to a valid absolute per-user runtime directory.");
    }

    const std::filesystem::path runtimePath{*runtimeDirectory};
    if (!runtimePath.is_absolute()) {
        return invalidEnvironment<std::filesystem::path>(
            "XDG_RUNTIME_DIR must be absolute.",
            "Set XDG_RUNTIME_DIR to an absolute per-user runtime directory.");
    }

    return Result<std::filesystem::path>::success(
        (runtimePath / prismdrakeDirectory).lexically_normal());
}

[[nodiscard]] std::optional<std::string> environmentValue(const char *name) {
    if (const char *value = std::getenv(name); value != nullptr) {
        return std::string{value};
    }
    return std::nullopt;
}

[[nodiscard]] Result<void> validatePrivateRuntimeDirectory(const std::filesystem::path &directory,
                                                           std::string_view description,
                                                           std::uintmax_t expectedUserId) {
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return Result<void>::failure(Error{
                ErrorCode::not_found,
                std::string{description} + " does not exist.",
                "Create the runtime directory as the current user with private mode 0700.",
            });
        }
        return Result<void>::failure(Error{
            ErrorCode::io_error,
            std::string{"Could not inspect "} + std::string{description} + ".",
            "Check runtime-directory accessibility and retry.",
        });
    }

    if (S_ISLNK(status.st_mode)) {
        return invalidEnvironment<void>(
            std::string{description} + " must not be a symbolic link.",
            "Use a private directory owned by the current user instead of a symbolic link.");
    }
    if (!S_ISDIR(status.st_mode)) {
        return invalidEnvironment<void>(
            std::string{description} + " is not a directory.",
            "Replace it with a private directory owned by the current user.");
    }
    if (static_cast<std::uintmax_t>(status.st_uid) != expectedUserId) {
        return Result<void>::failure(Error{
            ErrorCode::permission_denied,
            std::string{description} + " is owned by an unexpected user.",
            "Use a runtime directory owned by the current user.",
        });
    }
    if ((status.st_mode & 07777U) != 0700U) {
        return Result<void>::failure(Error{
            ErrorCode::permission_denied,
            std::string{description} + " must have private mode 0700.",
            "Set the runtime directory permissions to 0700 before starting Prismdrake.",
        });
    }

    return Result<void>::success();
}

} // namespace

Result<XdgPaths> resolveXdgPaths(const XdgEnvironment &environment) {
    auto configDirectory =
        resolveHomeBasedDirectory({environment.configHome, environment.home, ".config"});
    if (!configDirectory) {
        return Result<XdgPaths>::failure(configDirectory.error());
    }

    auto dataDirectory =
        resolveHomeBasedDirectory({environment.dataHome, environment.home, ".local/share"});
    if (!dataDirectory) {
        return Result<XdgPaths>::failure(dataDirectory.error());
    }

    auto stateDirectory =
        resolveHomeBasedDirectory({environment.stateHome, environment.home, ".local/state"});
    if (!stateDirectory) {
        return Result<XdgPaths>::failure(stateDirectory.error());
    }

    auto cacheDirectory =
        resolveHomeBasedDirectory({environment.cacheHome, environment.home, ".cache"});
    if (!cacheDirectory) {
        return Result<XdgPaths>::failure(cacheDirectory.error());
    }

    auto runtimeDirectory = resolveRuntimeDirectory(environment.runtimeDirectory);
    if (!runtimeDirectory) {
        return Result<XdgPaths>::failure(runtimeDirectory.error());
    }

    return Result<XdgPaths>::success(XdgPaths{
        std::move(configDirectory).value(),
        std::move(dataDirectory).value(),
        std::move(stateDirectory).value(),
        std::move(cacheDirectory).value(),
        std::move(runtimeDirectory).value(),
    });
}

XdgEnvironment currentProcessXdgEnvironment() {
    return XdgEnvironment{
        environmentValue("HOME"),           environmentValue("XDG_CONFIG_HOME"),
        environmentValue("XDG_DATA_HOME"),  environmentValue("XDG_STATE_HOME"),
        environmentValue("XDG_CACHE_HOME"), environmentValue("XDG_RUNTIME_DIR"),
    };
}

Result<XdgPaths> resolveCurrentProcessXdgPaths() {
    return resolveXdgPaths(currentProcessXdgEnvironment());
}

Result<void>
validateRuntimeDirectoryBoundary(const std::filesystem::path &runtimeBaseDirectory,
                                 const std::filesystem::path &prismdrakeRuntimeDirectory,
                                 std::uintmax_t expectedUserId) {
    if (!runtimeBaseDirectory.is_absolute() || !prismdrakeRuntimeDirectory.is_absolute()) {
        return invalidEnvironment<void>(
            "Runtime directory validation requires absolute paths.",
            "Resolve XDG_RUNTIME_DIR before validating the runtime directory boundary.");
    }

    const auto normalizedBase = runtimeBaseDirectory.lexically_normal();
    const auto normalizedPrismdrake = prismdrakeRuntimeDirectory.lexically_normal();
    if (normalizedPrismdrake != (normalizedBase / prismdrakeDirectory).lexically_normal()) {
        return invalidEnvironment<void>(
            "The Prismdrake runtime directory is outside its expected XDG boundary.",
            "Use the prismdrake subdirectory directly beneath XDG_RUNTIME_DIR.");
    }

    auto baseResult = validatePrivateRuntimeDirectory(normalizedBase, "XDG runtime base directory",
                                                      expectedUserId);
    if (!baseResult) {
        return baseResult;
    }
    return validatePrivateRuntimeDirectory(normalizedPrismdrake, "Prismdrake runtime subdirectory",
                                           expectedUserId);
}

std::uintmax_t currentProcessUserId() noexcept { return static_cast<std::uintmax_t>(::geteuid()); }

Result<void> validateCurrentProcessRuntimeDirectory(const XdgPaths &paths) {
    return validateRuntimeDirectoryBoundary(paths.runtimeDirectory.parent_path(),
                                            paths.runtimeDirectory, currentProcessUserId());
}

} // namespace prismdrake::foundation
