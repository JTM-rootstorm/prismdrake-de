#include "ConfigurationLoader.hpp"

#include "AtomicFile.hpp"
#include "BoundedFile.hpp"

#include <string_view>
#include <utility>

namespace prismdrake::config {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<ConfigurationCandidate> loadSource(const std::filesystem::path &path,
                                                        ConfigurationSource source,
                                                        ConfigurationParseOptions options) {
    auto document = foundation::readBoundedFile(path, maximumConfigurationBytes);
    if (!document) {
        return Result<ConfigurationCandidate>::failure(std::move(document).error());
    }

    auto configuration = parseConfigurationToml(document.value(), options);
    if (!configuration) {
        return Result<ConfigurationCandidate>::failure(std::move(configuration).error());
    }

    return Result<ConfigurationCandidate>::success(ConfigurationCandidate{
        std::move(configuration).value(), std::move(document).value(), source});
}

[[nodiscard]] Error noValidSourceError(const Error &packagedError) {
    return {packagedError.code,
            "No complete valid configuration source is available; " + packagedError.message,
            "Repair the packaged default or restore a validated user configuration. " +
                packagedError.recovery};
}

} // namespace

ConfigurationLocations configurationLocations(const foundation::XdgPaths &xdgPaths,
                                              std::filesystem::path packagedDefault) {
    return {
        xdgPaths.configDirectory / "config.toml",
        xdgPaths.stateDirectory / "last-known-valid-config.toml",
        std::move(packagedDefault),
    };
}

Result<StartupConfiguration> loadStartupConfiguration(const ConfigurationLocations &locations,
                                                      ConfigurationParseOptions options) {
    auto user = loadSource(locations.user, ConfigurationSource::user, options);
    if (user) {
        return Result<StartupConfiguration>::success(
            StartupConfiguration{std::move(user).value(), {}});
    }

    std::vector<ConfigurationIssue> issues;
    const bool userMissing = user.error().code == ErrorCode::not_found;
    if (!userMissing) {
        issues.push_back({ConfigurationSource::user, user.error()});

        auto lastKnownValid =
            loadSource(locations.lastKnownValid, ConfigurationSource::last_known_valid, options);
        if (lastKnownValid) {
            return Result<StartupConfiguration>::success(
                StartupConfiguration{std::move(lastKnownValid).value(), std::move(issues)});
        }
        if (lastKnownValid.error().code != ErrorCode::not_found) {
            issues.push_back({ConfigurationSource::last_known_valid, lastKnownValid.error()});
        }
    }

    auto packaged =
        loadSource(locations.packagedDefault, ConfigurationSource::packaged_default, options);
    if (!packaged) {
        return Result<StartupConfiguration>::failure(noValidSourceError(packaged.error()));
    }
    return Result<StartupConfiguration>::success(
        StartupConfiguration{std::move(packaged).value(), std::move(issues)});
}

Result<ConfigurationCandidate> loadReloadConfiguration(const ConfigurationLocations &locations,
                                                       ConfigurationParseOptions options) {
    auto user = loadSource(locations.user, ConfigurationSource::user, options);
    if (user) {
        return user;
    }
    if (user.error().code != ErrorCode::not_found) {
        return Result<ConfigurationCandidate>::failure(std::move(user).error());
    }
    return loadSource(locations.packagedDefault, ConfigurationSource::packaged_default, options);
}

Result<ConfigurationCandidate> loadPackagedConfiguration(const ConfigurationLocations &locations,
                                                         ConfigurationParseOptions options) {
    return loadSource(locations.packagedDefault, ConfigurationSource::packaged_default, options);
}

Result<Configuration> validateAndWriteUserConfiguration(const ConfigurationLocations &locations,
                                                        std::string_view input,
                                                        ConfigurationParseOptions options) {
    auto configuration = parseConfigurationToml(input, options);
    if (!configuration) {
        return configuration;
    }

    auto write = foundation::writeFileAtomically(locations.user, input);
    if (!write) {
        return Result<Configuration>::failure(std::move(write).error());
    }
    return configuration;
}

Result<void> promoteLastKnownValidConfiguration(const ConfigurationLocations &locations,
                                                const ConfigurationCandidate &candidate,
                                                ConfigurationParseOptions options) {
    if (candidate.source != ConfigurationSource::user) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument,
             "Only a validated user candidate can become last-known-valid state.",
             "Publish the complete user-derived settings and theme snapshot before promotion."});
    }

    // ConfigurationCandidate is deliberately a simple value that can cross later publication
    // boundaries. Revalidate its retained bytes here so a stale or forged value can never corrupt
    // recoverable state, even if a caller mutates or reconstructs the aggregate.
    auto revalidated = parseConfigurationToml(candidate.originalDocument, options);
    if (!revalidated) {
        return Result<void>::failure(std::move(revalidated).error());
    }
    if (revalidated.value() != candidate.configuration) {
        return Result<void>::failure(
            {ErrorCode::validation_error,
             "The retained configuration document does not match the published candidate.",
             "Promote the exact validated document and normalized configuration together."});
    }
    return foundation::writeFileAtomically(locations.lastKnownValid, candidate.originalDocument);
}

} // namespace prismdrake::config
