#include "ServiceOptions.hpp"

#include "BuildInfo.hpp"

#include <filesystem>

namespace prismdrake::notifyd {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

template <typename T> [[nodiscard]] Result<T> invalidUsage(std::string message) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_argument, std::move(message), "Use --help for accepted options."});
}

} // namespace

Result<ServiceOptions> parseServiceOptions(std::span<const std::string_view> arguments,
                                           const foundation::XdgEnvironment &environment) {
    ServiceAction action = ServiceAction::run;
    bool foregroundSeen = false;
    for (const auto argument : arguments) {
        if (argument == "--foreground") {
            if (foregroundSeen || action != ServiceAction::run) {
                return invalidUsage<ServiceOptions>("A notifyd option was combined incorrectly.");
            }
            foregroundSeen = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            if (action != ServiceAction::run || foregroundSeen) {
                return invalidUsage<ServiceOptions>("A notifyd action was combined incorrectly.");
            }
            action = ServiceAction::show_help;
            continue;
        }
        if (argument == "--version" || argument == "-V") {
            if (action != ServiceAction::run || foregroundSeen) {
                return invalidUsage<ServiceOptions>("A notifyd action was combined incorrectly.");
            }
            action = ServiceAction::show_version;
            continue;
        }
        return invalidUsage<ServiceOptions>("An unsupported notifyd argument was provided.");
    }

    if (action != ServiceAction::run) {
        return Result<ServiceOptions>::success({action, {}});
    }
    auto paths = foundation::resolveXdgPaths(environment);
    if (!paths) {
        return Result<ServiceOptions>::failure(paths.error());
    }
    AuthorityOptions authority;
    authority.persistentHistoryPath =
        paths.value().stateDirectory / "notifications" / "history-v1.json";
    return Result<ServiceOptions>::success({ServiceAction::run, std::move(authority)});
}

std::string serviceHelpText() {
    return "Usage: prismdrake-notifyd [--foreground] [--help] [--version]\n"
           "\n"
           "Provide the bounded Prismdrake notification service on the user session bus.\n"
           "\n"
           "Options:\n"
           "  --foreground  Run in the foreground (the default).\n"
           "  -h, --help    Show this help and exit.\n"
           "  -V, --version Show the version and exit.\n";
}

std::string serviceVersionText() {
    return "prismdrake-notifyd " + std::string{foundation::productVersion()} + '\n';
}

} // namespace prismdrake::notifyd
