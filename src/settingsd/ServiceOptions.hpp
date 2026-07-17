#pragma once

#include "ConfigurationLoader.hpp"
#include "Result.hpp"
#include "SettingsEngine.hpp"
#include "XdgPaths.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace prismdrake::settingsd {

/// Process actions accepted by the settings daemon command-line interface.
enum class ServiceAction : std::uint8_t {
    run,
    show_help,
    show_version,
};

/// Distribution-owned paths compiled into prismdrake-settingsd.
///
/// Callers provide these from build-system definitions. They are not command-line
/// options and must never be populated from a D-Bus request.
struct ServicePathDefaults final {
    std::filesystem::path packagedConfiguration;
    std::filesystem::path themeDirectory;
};

/// Fully resolved local paths required to start the settings engine.
struct ServiceRuntimeOptions final {
    foundation::XdgPaths xdgPaths;
    settings::SettingsEngineOptions settingsEngine;
};

/// Parsed process options. Runtime paths are populated only for the run action.
struct ServiceOptions final {
    ServiceAction action = ServiceAction::run;
    bool foreground = true;
    std::optional<ServiceRuntimeOptions> runtime;
};

/// Parses arguments excluding argv[0] and resolves current-process XDG paths for
/// the run action. Unknown options, positional arguments, and conflicting actions
/// are rejected without reflecting argument contents in the returned error.
[[nodiscard]] foundation::Result<ServiceOptions>
parseServiceOptions(std::span<const std::string_view> arguments,
                    const ServicePathDefaults &pathDefaults);

/// Returns stable, path-free command-line help text.
[[nodiscard]] std::string serviceHelpText();

/// Returns the tracked product version in a stable, path-free format.
[[nodiscard]] std::string serviceVersionText();

} // namespace prismdrake::settingsd
