#pragma once

#include "ConfigurationParser.hpp"
#include "Result.hpp"
#include "XdgPaths.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace prismdrake::config {

enum class ConfigurationSource : std::uint8_t {
    user,
    last_known_valid,
    packaged_default,
};

/// Stable source identity plus one redacted failure from an attempted source.
struct ConfigurationIssue final {
    ConfigurationSource source;
    foundation::Error error;

    friend bool operator==(const ConfigurationIssue &, const ConfigurationIssue &) = default;
};

/// Filesystem locations selected by XDG resolution and distribution packaging.
struct ConfigurationLocations final {
    std::filesystem::path user;
    std::filesystem::path lastKnownValid;
    std::filesystem::path packagedDefault;
};

/// Complete validated input that has not yet been assigned a publication generation.
struct ConfigurationCandidate final {
    Configuration configuration;
    std::string originalDocument;
    ConfigurationSource source;
};

/// Startup result with ordered, redacted failures that preceded recovery.
struct StartupConfiguration final {
    ConfigurationCandidate candidate;
    std::vector<ConfigurationIssue> issues;
};

/// Selects stable internal filenames beneath resolved XDG directories.
[[nodiscard]] ConfigurationLocations configurationLocations(const foundation::XdgPaths &xdgPaths,
                                                            std::filesystem::path packagedDefault);

/// Loads startup state: user, then LKV for invalid user input, then packaged default.
/// A missing user file intentionally resets to the packaged default without consulting LKV.
[[nodiscard]] foundation::Result<StartupConfiguration>
loadStartupConfiguration(const ConfigurationLocations &locations,
                         ConfigurationParseOptions options = {});

/// Loads a reload candidate. Invalid input returns failure so the caller retains its current
/// generation; a missing user file explicitly selects the packaged default.
[[nodiscard]] foundation::Result<ConfigurationCandidate>
loadReloadConfiguration(const ConfigurationLocations &locations,
                        ConfigurationParseOptions options = {});

/// Validates one complete document and atomically replaces only the canonical user file.
[[nodiscard]] foundation::Result<Configuration>
validateAndWriteUserConfiguration(const ConfigurationLocations &locations, std::string_view input,
                                  ConfigurationParseOptions options = {});

/// Persists exact validated user TOML only when it still normalizes to the published candidate.
[[nodiscard]] foundation::Result<void>
promoteLastKnownValidConfiguration(const ConfigurationLocations &locations,
                                   const ConfigurationCandidate &candidate,
                                   ConfigurationParseOptions options = {});

} // namespace prismdrake::config
