#include "ApplicationPaths.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view defaultDataDirectories = "/usr/local/share:/usr/share";
constexpr std::string_view applicationsDirectory = "applications";

[[nodiscard]] Result<ApplicationPaths> failure(ErrorCode code, std::string message,
                                               std::string recovery) {
    return Result<ApplicationPaths>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool containsNullByte(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<void> validateInputBounds(const ApplicationPathEnvironment &environment) {
    const std::array values{&environment.home, &environment.dataHome, &environment.dataDirectories};
    std::size_t totalBytes = 0U;
    for (const auto *value : values) {
        if (!*value) {
            continue;
        }
        if ((*value)->size() > maximumApplicationPathValueBytes ||
            totalBytes > maximumApplicationPathEnvironmentBytes - (*value)->size()) {
            return Result<void>::failure(
                {ErrorCode::too_large, "The application-path environment exceeds its byte limit.",
                 "Use smaller XDG data-directory environment values."});
        }
        if (containsNullByte(**value)) {
            return Result<void>::failure(
                {ErrorCode::invalid_environment,
                 "The application-path environment contains an invalid null byte.",
                 "Use valid absolute XDG data-directory paths."});
        }
        totalBytes += (*value)->size();
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::filesystem::path>
resolveDataHome(const ApplicationPathEnvironment &environment) {
    if (environment.dataHome && !environment.dataHome->empty()) {
        const std::filesystem::path configured{*environment.dataHome};
        if (configured.is_absolute()) {
            return Result<std::filesystem::path>::success(configured.lexically_normal());
        }
    }

    if (!environment.home || environment.home->empty()) {
        return Result<std::filesystem::path>::failure(
            {ErrorCode::invalid_environment,
             "HOME is required when the XDG data-home default is needed.",
             "Set HOME or XDG_DATA_HOME to a valid absolute path."});
    }
    const std::filesystem::path home{*environment.home};
    if (!home.is_absolute()) {
        return Result<std::filesystem::path>::failure(
            {ErrorCode::invalid_environment,
             "HOME must be absolute when the XDG data-home default is needed.",
             "Set HOME or XDG_DATA_HOME to a valid absolute path."});
    }
    return Result<std::filesystem::path>::success((home / ".local/share").lexically_normal());
}

void appendUnique(std::vector<std::filesystem::path> &directories,
                  const std::filesystem::path &dataDirectory) {
    auto applicationDirectory = (dataDirectory / applicationsDirectory).lexically_normal();
    if (std::ranges::find(directories, applicationDirectory) == directories.end()) {
        directories.push_back(std::move(applicationDirectory));
    }
}

[[nodiscard]] std::optional<std::string> environmentValue(const char *name) {
    if (const char *value = std::getenv(name); value != nullptr) {
        return std::string{value};
    }
    return std::nullopt;
}

} // namespace

Result<ApplicationPaths> resolveApplicationPaths(const ApplicationPathEnvironment &environment) {
    auto bounds = validateInputBounds(environment);
    if (!bounds) {
        return Result<ApplicationPaths>::failure(std::move(bounds).error());
    }

    auto dataHome = resolveDataHome(environment);
    if (!dataHome) {
        return Result<ApplicationPaths>::failure(std::move(dataHome).error());
    }

    std::vector<std::filesystem::path> directories;
    directories.reserve(3U);
    appendUnique(directories, std::move(dataHome).value());

    const std::string_view configuredDirectories =
        !environment.dataDirectories || environment.dataDirectories->empty()
            ? defaultDataDirectories
            : std::string_view{*environment.dataDirectories};

    std::size_t componentCount = 0U;
    std::size_t offset = 0U;
    while (offset <= configuredDirectories.size()) {
        if (++componentCount > maximumApplicationDataDirectoryEntries) {
            return failure(ErrorCode::too_large,
                           "XDG_DATA_DIRS contains too many directory entries.",
                           "Use a smaller bounded XDG_DATA_DIRS list.");
        }

        const auto separator = configuredDirectories.find(':', offset);
        const auto length = separator == std::string_view::npos
                                ? configuredDirectories.size() - offset
                                : separator - offset;
        const auto component = configuredDirectories.substr(offset, length);
        if (!component.empty()) {
            const std::filesystem::path dataDirectory{component};
            if (dataDirectory.is_absolute()) {
                appendUnique(directories, dataDirectory.lexically_normal());
            }
        }

        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }

    return Result<ApplicationPaths>::success(ApplicationPaths{std::move(directories)});
}

ApplicationPathEnvironment currentProcessApplicationPathEnvironment() {
    return ApplicationPathEnvironment{environmentValue("HOME"), environmentValue("XDG_DATA_HOME"),
                                      environmentValue("XDG_DATA_DIRS")};
}

Result<ApplicationPaths> resolveCurrentProcessApplicationPaths() {
    return resolveApplicationPaths(currentProcessApplicationPathEnvironment());
}

} // namespace prismdrake::launcher
