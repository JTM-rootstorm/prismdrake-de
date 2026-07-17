#include "SessionOptions.hpp"

#include "BuildInfo.hpp"

#include <cerrno>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::session {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class PathSource : std::uint8_t {
    argument,
    compiled_default,
};

template <typename T>
[[nodiscard]] Result<T> invalidUsage(std::string message, std::string recovery) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_argument, std::move(message), std::move(recovery)});
}

template <typename T>
[[nodiscard]] Result<T> invalidDefault(std::string message, std::string recovery) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_environment, std::move(message), std::move(recovery)});
}

template <typename T>
[[nodiscard]] Result<T> pathFailure(PathSource source, ErrorCode code, std::string message,
                                    std::string recovery) {
    if (source == PathSource::argument) {
        return Result<T>::failure(
            Error{code == ErrorCode::invalid_environment ? ErrorCode::invalid_argument : code,
                  std::move(message), std::move(recovery)});
    }
    return invalidDefault<T>(std::move(message), std::move(recovery));
}

[[nodiscard]] bool containsNullByte(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<std::filesystem::path>
validateExecutablePath(const std::filesystem::path &path, PathSource source) {
    const auto native = path.native();
    if (path.empty() || native.size() > maximumSessionExecutablePathBytes ||
        containsNullByte(native) || !path.is_absolute()) {
        return pathFailure<std::filesystem::path>(
            source, ErrorCode::invalid_environment,
            "A required component executable location is invalid.",
            "Use an absolute executable path no longer than the supported path limit.");
    }

    const auto normalized = path.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute() || !normalized.has_filename() ||
        normalized == normalized.root_path()) {
        return pathFailure<std::filesystem::path>(
            source, ErrorCode::invalid_environment,
            "A required component executable location is invalid.",
            "Use an absolute path naming one component executable.");
    }

    struct stat status = {};
    if (::stat(normalized.c_str(), &status) != 0) {
        const auto code = errno == ENOENT || errno == ENOTDIR ? ErrorCode::not_found
                          : errno == EACCES                   ? ErrorCode::permission_denied
                                                              : ErrorCode::io_error;
        return pathFailure<std::filesystem::path>(
            source, code, "A required component executable is unavailable.",
            "Install or build the component and retry with its absolute executable path.");
    }
    if (!S_ISREG(status.st_mode)) {
        return pathFailure<std::filesystem::path>(
            source, ErrorCode::unsupported,
            "A required component executable location is not a regular file.",
            "Select an absolute path naming a regular executable file.");
    }
    if (::access(normalized.c_str(), X_OK) != 0) {
        const auto code = errno == EACCES ? ErrorCode::permission_denied : ErrorCode::io_error;
        return pathFailure<std::filesystem::path>(
            source, code, "A required component file is not executable.",
            "Grant appropriate execute permission or select another component executable.");
    }

    return Result<std::filesystem::path>::success(normalized);
}

[[nodiscard]] Result<SessionAction> selectAction(SessionAction current, SessionAction requested) {
    if (current != SessionAction::run || requested == SessionAction::run) {
        return invalidUsage<SessionAction>("Conflicting command-line actions were provided.",
                                           "Select at most one of --help or --version.");
    }
    return Result<SessionAction>::success(requested);
}

} // namespace

Result<SessionOptions> parseSessionOptions(std::span<const std::string_view> arguments,
                                           const SessionPathDefaults &pathDefaults) {
    SessionAction action = SessionAction::run;
    std::optional<std::filesystem::path> settingsd;
    std::optional<std::filesystem::path> shell;
    bool settingsdSeen = false;
    bool shellSeen = false;

    for (std::size_t index = 0U; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            auto selected = selectAction(action, SessionAction::show_help);
            if (!selected) {
                return Result<SessionOptions>::failure(selected.error());
            }
            action = selected.value();
            continue;
        }
        if (argument == "--version" || argument == "-V") {
            auto selected = selectAction(action, SessionAction::show_version);
            if (!selected) {
                return Result<SessionOptions>::failure(selected.error());
            }
            action = selected.value();
            continue;
        }

        const bool isSettingsd = argument == "--settingsd";
        const bool isShell = argument == "--shell";
        if (!isSettingsd && !isShell) {
            return invalidUsage<SessionOptions>(
                "An unsupported command-line argument was provided.",
                "Use --help to list the accepted options.");
        }
        if (action != SessionAction::run) {
            return invalidUsage<SessionOptions>(
                "A display action cannot be combined with a runtime option.",
                "Use --help or --version by itself.");
        }

        bool &seen = isSettingsd ? settingsdSeen : shellSeen;
        if (seen) {
            return invalidUsage<SessionOptions>(
                "A component executable option was provided more than once.",
                "Specify each of --settingsd and --shell at most once.");
        }
        seen = true;
        if (index + 1U >= arguments.size() || arguments[index + 1U].starts_with('-')) {
            return invalidUsage<SessionOptions>(
                "A component executable option is missing its value.",
                "Follow each component option with one absolute executable path.");
        }

        const auto value = arguments[++index];
        auto validated = validateExecutablePath(std::filesystem::path{value}, PathSource::argument);
        if (!validated) {
            return Result<SessionOptions>::failure(validated.error());
        }
        (isSettingsd ? settingsd : shell).emplace(std::move(validated).value());
    }

    if (action != SessionAction::run) {
        if (settingsdSeen || shellSeen) {
            return invalidUsage<SessionOptions>(
                "A display action cannot be combined with a runtime option.",
                "Use --help or --version by itself.");
        }
        return Result<SessionOptions>::success(SessionOptions{action, std::nullopt});
    }

    if (!settingsd) {
        if (pathDefaults.settingsdExecutable.empty()) {
            return invalidUsage<SessionOptions>(
                "The settings service executable was not specified.",
                "Specify --settingsd with an absolute executable path.");
        }
        auto validated =
            validateExecutablePath(pathDefaults.settingsdExecutable, PathSource::compiled_default);
        if (!validated) {
            return Result<SessionOptions>::failure(validated.error());
        }
        settingsd.emplace(std::move(validated).value());
    }
    if (!shell) {
        if (pathDefaults.shellExecutable.empty()) {
            return invalidUsage<SessionOptions>(
                "The shell executable was not specified.",
                "Specify --shell with an absolute executable path.");
        }
        auto validated =
            validateExecutablePath(pathDefaults.shellExecutable, PathSource::compiled_default);
        if (!validated) {
            return Result<SessionOptions>::failure(validated.error());
        }
        shell.emplace(std::move(validated).value());
    }

    return Result<SessionOptions>::success(
        SessionOptions{SessionAction::run, SessionRuntimeOptions{std::move(settingsd).value(),
                                                                 std::move(shell).value()}});
}

std::string sessionHelpText() {
    return "Usage: prismdrake-session [--settingsd PATH] [--shell PATH] [--help] [--version]\n"
           "\n"
           "Start and supervise the Prismdrake development session.\n"
           "\n"
           "Options:\n"
           "  --settingsd PATH  Use the absolute settings service executable path.\n"
           "  --shell PATH      Use the absolute shell executable path.\n"
           "  -h, --help        Show this help and exit.\n"
           "  -V, --version     Show the version and exit.\n";
}

std::string sessionVersionText() {
    return "prismdrake-session " + std::string{foundation::productVersion()} + '\n';
}

} // namespace prismdrake::session
