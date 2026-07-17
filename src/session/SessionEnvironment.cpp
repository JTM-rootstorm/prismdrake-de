#include "SessionEnvironment.hpp"

#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

extern char **environ;

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view displayName = "DISPLAY";
constexpr std::string_view runtimeName = "XDG_RUNTIME_DIR";
constexpr std::string_view busName = "DBUS_SESSION_BUS_ADDRESS";
constexpr std::array identityNames{std::string_view{"XDG_CURRENT_DESKTOP"},
                                   std::string_view{"XDG_SESSION_DESKTOP"},
                                   std::string_view{"DESKTOP_SESSION"}};
constexpr std::array identityEntries{std::string_view{"XDG_CURRENT_DESKTOP=Prismdrake"},
                                     std::string_view{"XDG_SESSION_DESKTOP=prismdrake"},
                                     std::string_view{"DESKTOP_SESSION=prismdrake"}};

[[nodiscard]] Result<PreparedSessionEnvironment> invalidEnvironment(std::string message,
                                                                    std::string recovery) {
    return Result<PreparedSessionEnvironment>::failure(
        {ErrorCode::invalid_environment, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool isIdentityName(std::string_view name) noexcept {
    for (const auto identity : identityNames) {
        if (name == identity) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Result<void> captureUnique(std::optional<std::string> &destination,
                                         std::string_view value) {
    if (destination) {
        return Result<void>::failure(
            {ErrorCode::invalid_environment,
             "A required session environment value is defined more than once.",
             "Provide exactly one display, runtime-directory, and session-bus value."});
    }
    if (value.empty()) {
        return Result<void>::failure(
            {ErrorCode::invalid_environment, "A required session environment value is empty.",
             "Provide a non-empty display, runtime-directory, and session-bus value."});
    }
    destination = std::string{value};
    return Result<void>::success();
}

} // namespace

Result<PreparedSessionEnvironment>
prepareSessionEnvironment(std::span<const std::string_view> inherited) {
    if (inherited.size() > maximumSessionEnvironmentEntries) {
        return invalidEnvironment("The inherited session environment has too many entries.",
                                  "Start Prismdrake with a smaller bounded environment.");
    }

    std::vector<std::string> entries;
    entries.reserve(inherited.size() + identityEntries.size());
    std::optional<std::string> display;
    std::optional<std::string> runtime;
    std::optional<std::string> bus;
    std::size_t totalBytes = 0U;

    for (const auto entry : inherited) {
        if (entry.empty() || entry.find('\0') != std::string_view::npos ||
            entry.size() > maximumSessionEnvironmentEntryBytes ||
            totalBytes > maximumSessionEnvironmentBytes - entry.size()) {
            return invalidEnvironment("The inherited session environment exceeds its byte limit.",
                                      "Start Prismdrake with smaller environment values.");
        }
        totalBytes += entry.size();

        const auto separator = entry.find('=');
        if (separator == std::string_view::npos || separator == 0U) {
            return invalidEnvironment(
                "The inherited session environment contains a malformed entry.",
                "Provide environment entries in non-empty NAME=value form.");
        }
        const auto name = entry.substr(0U, separator);
        const auto value = entry.substr(separator + 1U);
        Result<void> captured = Result<void>::success();
        if (name == displayName) {
            captured = captureUnique(display, value);
        } else if (name == runtimeName) {
            captured = captureUnique(runtime, value);
        } else if (name == busName) {
            captured = captureUnique(bus, value);
        }
        if (!captured) {
            return Result<PreparedSessionEnvironment>::failure(std::move(captured).error());
        }
        if (!isIdentityName(name)) {
            entries.emplace_back(entry);
        }
    }

    if (!display || !runtime || !bus) {
        return invalidEnvironment("A required development-session environment value is missing.",
                                  "Set DISPLAY, XDG_RUNTIME_DIR, and DBUS_SESSION_BUS_ADDRESS.");
    }
    const std::filesystem::path runtimePath{*runtime};
    if (!runtimePath.is_absolute()) {
        return invalidEnvironment("The session runtime directory is not an absolute path.",
                                  "Set XDG_RUNTIME_DIR to an absolute current-user directory.");
    }

    for (const auto identity : identityEntries) {
        entries.emplace_back(identity);
    }
    return Result<PreparedSessionEnvironment>::success(PreparedSessionEnvironment{
        std::move(entries), std::move(*display), runtimePath.lexically_normal(), std::move(*bus)});
}

Result<PreparedSessionEnvironment> prepareCurrentSessionEnvironment() {
    if (::environ == nullptr) {
        return invalidEnvironment("The current process environment is unavailable.",
                                  "Start Prismdrake with a valid process environment.");
    }
    std::vector<std::string_view> inherited;
    inherited.reserve(128U);
    for (std::size_t index = 0U; ::environ[index] != nullptr; ++index) {
        if (index >= maximumSessionEnvironmentEntries) {
            return invalidEnvironment("The current session environment has too many entries.",
                                      "Start Prismdrake with a smaller bounded environment.");
        }
        inherited.emplace_back(::environ[index]);
    }
    return prepareSessionEnvironment(inherited);
}

} // namespace prismdrake::session
