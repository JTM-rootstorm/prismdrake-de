#include "ServiceOptions.hpp"

#include "BuildInfo.hpp"

#include <utility>

namespace prismdrake::settingsd {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

template <typename T>
[[nodiscard]] Result<T> invalidUsage(std::string message, std::string recovery) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_argument, std::move(message), std::move(recovery)});
}

template <typename T>
[[nodiscard]] Result<T> invalidDefaults(std::string message, std::string recovery) {
    return Result<T>::failure(
        Error{ErrorCode::invalid_environment, std::move(message), std::move(recovery)});
}

[[nodiscard]] Result<ServicePathDefaults>
validatePathDefaults(const ServicePathDefaults &defaults) {
    if (defaults.packagedConfiguration.empty() || !defaults.packagedConfiguration.is_absolute()) {
        return invalidDefaults<ServicePathDefaults>(
            "The packaged configuration location is not a valid absolute path.",
            "Rebuild Prismdrake with an absolute packaged configuration location.");
    }
    if (defaults.themeDirectory.empty() || !defaults.themeDirectory.is_absolute()) {
        return invalidDefaults<ServicePathDefaults>(
            "The packaged theme location is not a valid absolute path.",
            "Rebuild Prismdrake with an absolute packaged theme location.");
    }

    return Result<ServicePathDefaults>::success(ServicePathDefaults{
        defaults.packagedConfiguration.lexically_normal(),
        defaults.themeDirectory.lexically_normal(),
    });
}

[[nodiscard]] Result<ServiceAction> selectAction(ServiceAction current, ServiceAction requested) {
    if (current != ServiceAction::run || requested == ServiceAction::run) {
        return invalidUsage<ServiceAction>("Conflicting command-line actions were provided.",
                                           "Select at most one of --help or --version.");
    }
    return Result<ServiceAction>::success(requested);
}

} // namespace

Result<ServiceOptions> parseServiceOptions(std::span<const std::string_view> arguments,
                                           const ServicePathDefaults &pathDefaults) {
    ServiceAction action = ServiceAction::run;
    bool foregroundSeen = false;

    for (const auto argument : arguments) {
        if (argument == "--foreground") {
            if (foregroundSeen) {
                return invalidUsage<ServiceOptions>(
                    "A command-line option was provided more than once.",
                    "Specify --foreground at most once.");
            }
            foregroundSeen = true;
            continue;
        }

        if (argument == "--help" || argument == "-h") {
            auto selected = selectAction(action, ServiceAction::show_help);
            if (!selected) {
                return Result<ServiceOptions>::failure(selected.error());
            }
            action = selected.value();
            continue;
        }

        if (argument == "--version" || argument == "-V") {
            auto selected = selectAction(action, ServiceAction::show_version);
            if (!selected) {
                return Result<ServiceOptions>::failure(selected.error());
            }
            action = selected.value();
            continue;
        }

        return invalidUsage<ServiceOptions>("An unsupported command-line argument was provided.",
                                            "Use --help to list the accepted options.");
    }

    if (action != ServiceAction::run) {
        if (foregroundSeen) {
            return invalidUsage<ServiceOptions>(
                "A display action cannot be combined with a runtime option.",
                "Use --help or --version by itself.");
        }
        return Result<ServiceOptions>::success(ServiceOptions{action, true, std::nullopt});
    }

    auto defaults = validatePathDefaults(pathDefaults);
    if (!defaults) {
        return Result<ServiceOptions>::failure(defaults.error());
    }

    auto xdgPaths = foundation::resolveCurrentProcessXdgPaths();
    if (!xdgPaths) {
        return Result<ServiceOptions>::failure(xdgPaths.error());
    }

    auto paths = std::move(xdgPaths).value();
    auto settingsOptions = settings::SettingsEngineOptions{
        config::configurationLocations(paths, defaults.value().packagedConfiguration),
        defaults.value().themeDirectory,
        {},
        {},
    };

    return Result<ServiceOptions>::success(ServiceOptions{
        ServiceAction::run,
        true,
        ServiceRuntimeOptions{std::move(paths), std::move(settingsOptions)},
    });
}

std::string serviceHelpText() {
    return "Usage: prismdrake-settingsd [--foreground] [--help] [--version]\n"
           "\n"
           "Publish validated Prismdrake settings snapshots on the user session bus.\n"
           "\n"
           "Options:\n"
           "  --foreground  Run in the foreground (the default).\n"
           "  -h, --help    Show this help and exit.\n"
           "  -V, --version Show the version and exit.\n";
}

std::string serviceVersionText() {
    return "prismdrake-settingsd " + std::string{foundation::productVersion()} + '\n';
}

} // namespace prismdrake::settingsd
