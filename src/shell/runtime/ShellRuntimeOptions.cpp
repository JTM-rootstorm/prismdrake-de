#include "ShellRuntimeOptions.hpp"

#include "ApplicationPaths.hpp"
#include "DesktopEntryVisibility.hpp"
#include "ProcessLaunch.hpp"
#include "SessionReadinessProtocol.hpp"

#include <QLocale>
#include <QString>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

[[nodiscard]] Result<std::vector<std::string_view>> currentEnvironmentEntries() {
    if (::environ == nullptr) {
        return Result<std::vector<std::string_view>>::failure(
            {ErrorCode::invalid_environment, "The shell process environment is unavailable.",
             "Start Prismdrake with a valid bounded process environment."});
    }
    std::vector<std::string_view> inherited;
    inherited.reserve(128U);
    for (std::size_t index = 0U; ::environ[index] != nullptr; ++index) {
        if (index >= prismdrake::launcher::maximumProcessLaunchEnvironmentEntries) {
            return Result<std::vector<std::string_view>>::failure(
                {ErrorCode::too_large, "The shell launch environment has too many entries.",
                 "Start Prismdrake with a smaller bounded process environment."});
        }
        inherited.emplace_back(::environ[index]);
    }
    return Result<std::vector<std::string_view>>::success(std::move(inherited));
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

SessionReadinessSignal::SessionReadinessSignal(SessionReadinessSignal &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

SessionReadinessSignal &SessionReadinessSignal::operator=(SessionReadinessSignal &&other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

SessionReadinessSignal::~SessionReadinessSignal() { close(); }

void SessionReadinessSignal::close() noexcept {
    if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
    }
}

Result<void> SessionReadinessSignal::publish() {
    if (!pending()) {
        return Result<void>::success();
    }
    const auto message = foundation::sessionReadinessMessage;
    ssize_t sent = -1;
    do {
        sent = ::write(descriptor_, &message, sizeof(message));
    } while (sent < 0 && errno == EINTR);
    close();
    if (sent != static_cast<ssize_t>(sizeof(message))) {
        return Result<void>::failure(
            {ErrorCode::io_error, "The shell readiness signal could not be published.",
             "Restart the exact shell child through bounded session recovery."});
    }
    return Result<void>::success();
}

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
    bool foundReadinessDescriptor = false;
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
        if (entry.starts_with(foundation::sessionReadinessDescriptorEnvironment) &&
            entry.size() > foundation::sessionReadinessDescriptorEnvironment.size() &&
            entry[foundation::sessionReadinessDescriptorEnvironment.size()] == '=') {
            if (foundReadinessDescriptor) {
                return Result<std::vector<std::string>>::failure(
                    {ErrorCode::invalid_environment,
                     "The private shell readiness environment is duplicated.",
                     "Start prismdrake-shell through one exact session launch."});
            }
            foundReadinessDescriptor = true;
            continue;
        }
        result.emplace_back(entry);
    }
    return Result<std::vector<std::string>>::success(std::move(result));
}

Result<SessionReadinessSignal>
sessionReadinessSignalFromEnvironment(std::span<const std::string_view> inherited) {
    std::optional<int> descriptor;
    for (const auto entry : inherited) {
        const auto name = foundation::sessionReadinessDescriptorEnvironment;
        if (!entry.starts_with(name) || entry.size() <= name.size() || entry[name.size()] != '=') {
            continue;
        }
        if (descriptor) {
            return Result<SessionReadinessSignal>::failure(
                {ErrorCode::invalid_environment,
                 "The private shell readiness environment is duplicated.",
                 "Start prismdrake-shell through one exact session launch."});
        }
        const auto value = entry.substr(name.size() + 1U);
        int parsed = -1;
        const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (value.empty() || converted.ec != std::errc{} ||
            converted.ptr != value.data() + value.size() || parsed < 3) {
            return Result<SessionReadinessSignal>::failure(
                {ErrorCode::invalid_environment,
                 "The private shell readiness descriptor is invalid.",
                 "Start prismdrake-shell through the matching session supervisor."});
        }
        descriptor = parsed;
    }
    if (!descriptor) {
        return Result<SessionReadinessSignal>::success(SessionReadinessSignal{});
    }

    int descriptorFlags = -1;
    do {
        descriptorFlags = ::fcntl(*descriptor, F_GETFD);
    } while (descriptorFlags < 0 && errno == EINTR);
    struct stat descriptorState{};
    int descriptorResult = -1;
    do {
        descriptorResult = ::fstat(*descriptor, &descriptorState);
    } while (descriptorResult < 0 && errno == EINTR);
    if (descriptorFlags < 0 || descriptorResult < 0 || (descriptorState.st_mode & S_IFMT) != 0) {
        return Result<SessionReadinessSignal>::failure(
            {ErrorCode::invalid_environment,
             "The private shell readiness descriptor has the wrong type.",
             "Start prismdrake-shell through the matching session supervisor."});
    }
    int bounded = -1;
    do {
        bounded = ::fcntl(*descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC);
    } while (bounded < 0 && errno == EINTR);
    if (bounded < 0) {
        static_cast<void>(::close(*descriptor));
        return Result<SessionReadinessSignal>::failure(
            {ErrorCode::io_error, "The private shell readiness descriptor could not be bounded.",
             "Restart prismdrake-shell after checking process descriptor limits."});
    }
    return Result<SessionReadinessSignal>::success(SessionReadinessSignal{*descriptor});
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
    auto inherited = currentEnvironmentEntries();
    if (!inherited) {
        return Result<ShellRuntimeOptions>::failure(inherited.error());
    }
    auto environment = validatedLaunchEnvironment(inherited.value());
    if (!environment) {
        return Result<ShellRuntimeOptions>::failure(environment.error());
    }
    auto readiness = sessionReadinessSignalFromEnvironment(inherited.value());
    if (!readiness) {
        return Result<ShellRuntimeOptions>::failure(readiness.error());
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
    return Result<ShellRuntimeOptions>::success(ShellRuntimeOptions{
        std::move(display), std::move(launcherOptions), std::move(readiness).value()});
}

} // namespace prismdrake::shell::runtime
