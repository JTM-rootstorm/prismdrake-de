#include "ShellRuntimeOptions.hpp"

#include "ApplicationPaths.hpp"
#include "DesktopEntryVisibility.hpp"
#include "ProcessLaunch.hpp"

#include <QLocale>
#include <QString>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

extern char **environ;

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::size_t maximumDisplayBytes = 4096U;

[[nodiscard]] std::string environmentValue(const char *name, std::string fallback = {}) {
    if (const auto *value = std::getenv(name); value != nullptr) {
        return value;
    }
    return fallback;
}

[[nodiscard]] Result<std::vector<std::string>> currentLaunchEnvironment() {
    if (::environ == nullptr) {
        return Result<std::vector<std::string>>::failure(
            {ErrorCode::invalid_environment, "The shell process environment is unavailable.",
             "Start Prismdrake with a valid bounded process environment."});
    }
    std::vector<std::string_view> inherited;
    inherited.reserve(128U);
    for (std::size_t index = 0U; ::environ[index] != nullptr; ++index) {
        if (index >= prismdrake::launcher::maximumProcessLaunchEnvironmentEntries) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::too_large, "The shell launch environment has too many entries.",
                 "Start Prismdrake with a smaller bounded process environment."});
        }
        inherited.emplace_back(::environ[index]);
    }
    return validatedLaunchEnvironment(inherited);
}

[[nodiscard]] Result<std::filesystem::path> workingDirectory() {
    std::error_code error;
    auto current = std::filesystem::current_path(error);
    if (error || current.empty() || !current.is_absolute()) {
        return Result<std::filesystem::path>::failure(
            {ErrorCode::invalid_environment,
             "The shell process has no usable absolute working directory.",
             "Start Prismdrake from a valid session working directory."});
    }
    return Result<std::filesystem::path>::success(current.lexically_normal());
}

} // namespace

Result<std::vector<std::string>>
validatedLaunchEnvironment(std::span<const std::string_view> inherited) {
    if (inherited.size() > prismdrake::launcher::maximumProcessLaunchEnvironmentEntries) {
        return Result<std::vector<std::string>>::failure(
            {ErrorCode::too_large, "The shell launch environment has too many entries.",
             "Start Prismdrake with a smaller bounded process environment."});
    }

    std::vector<std::string> result;
    result.reserve(inherited.size());
    std::size_t totalBytes = 0U;
    for (const auto entry : inherited) {
        const auto separator = entry.find('=');
        if (entry.empty() || entry.find('\0') != std::string_view::npos || separator == 0U ||
            separator == std::string_view::npos) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::invalid_environment, "The shell launch environment is malformed.",
                 "Start Prismdrake with bounded NAME=value environment entries."});
        }
        if (entry.size() > prismdrake::launcher::maximumProcessLaunchEnvironmentEntryBytes ||
            totalBytes >
                prismdrake::launcher::maximumProcessLaunchEnvironmentBytes - entry.size() - 1U) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::too_large, "The shell launch environment exceeds its byte limit.",
                 "Start Prismdrake with smaller bounded environment values."});
        }
        totalBytes += entry.size() + 1U;
        result.emplace_back(entry);
    }
    return Result<std::vector<std::string>>::success(std::move(result));
}

Result<ShellRuntimeOptions> currentShellRuntimeOptions() {
    auto display = environmentValue("DISPLAY");
    if (display.empty() || display.size() > maximumDisplayBytes) {
        return Result<ShellRuntimeOptions>::failure(
            {ErrorCode::invalid_environment, "The X11 DISPLAY value is absent or oversized.",
             "Start prismdrake-shell inside the selected bounded X11 session."});
    }

    auto applicationPaths = prismdrake::launcher::resolveCurrentProcessApplicationPaths();
    if (!applicationPaths) {
        return Result<ShellRuntimeOptions>::failure(applicationPaths.error());
    }
    auto desktop = prismdrake::launcher::parseCurrentDesktopContext(
        environmentValue("XDG_CURRENT_DESKTOP", "Prismdrake"));
    if (!desktop) {
        return Result<ShellRuntimeOptions>::failure(desktop.error());
    }
    auto environment = currentLaunchEnvironment();
    if (!environment) {
        return Result<ShellRuntimeOptions>::failure(environment.error());
    }
    auto working = workingDirectory();
    if (!working) {
        return Result<ShellRuntimeOptions>::failure(working.error());
    }

    auto path = environmentValue("PATH", "/usr/local/bin:/usr/bin:/bin");
    prismdrake::launcher::DesktopExecutableLookupContext executableLookup{std::move(path),
                                                                          working.value()};
    prismdrake::launcher::ProcessLaunchContext launchContext{
        executableLookup, working.value(), std::move(environment).value(), std::nullopt};
    launcher::controller::LauncherControllerOptions launcherOptions{
        std::move(applicationPaths).value(),
        {QLocale::system().name().toStdString()},
        std::move(desktop).value(),
        {},
        executableLookup,
        std::move(launchContext)};
    return Result<ShellRuntimeOptions>::success(
        ShellRuntimeOptions{std::move(display), std::move(launcherOptions)});
}

} // namespace prismdrake::shell::runtime
