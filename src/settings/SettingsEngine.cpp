#include "SettingsEngine.hpp"

#include "ConfigurationParser.hpp"

#include <memory>
#include <utility>

namespace prismdrake::settings {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] std::vector<SettingsWarning>
startupWarnings(const std::vector<config::ConfigurationIssue> &issues) {
    std::vector<SettingsWarning> warnings;
    warnings.reserve(issues.size());
    for (const auto &issue : issues) {
        const auto warning = issue.source == config::ConfigurationSource::user
                                 ? SettingsWarning::invalid_user_configuration
                                 : SettingsWarning::invalid_last_known_valid_configuration;
        if (warnings.empty() || warnings.back() != warning) {
            warnings.push_back(warning);
        }
    }
    return warnings;
}

[[nodiscard]] ValidationDiagnostic invalidCandidateDiagnostic(const foundation::Error &error) {
    std::string code = "invalid_configuration";
    std::string recovery = "review_configuration";
    if (error.code == ErrorCode::syntax_error) {
        code = "syntax_error";
        recovery = "fix_toml_syntax";
    } else if (error.code == ErrorCode::unsupported) {
        code = "unsupported_schema_version";
        recovery = "use_supported_schema_version";
    }
    return {"candidate", "$", std::move(code), "complete_version_1_configuration",
            std::move(recovery)};
}

} // namespace

Result<std::unique_ptr<SettingsEngine>> SettingsEngine::start(SettingsEngineOptions options) {
    const bool safeMode = options.mode == SettingsEngineMode::development_safe_mode;
    options.themeOptions.safeMode = safeMode;
    auto bundle = theme::loadPackagedThemeBundle(options.themeDirectory);
    if (!bundle) {
        return Result<std::unique_ptr<SettingsEngine>>::failure(std::move(bundle).error());
    }
    Result<config::StartupConfiguration> startup = [&]() {
        if (!safeMode) {
            return config::loadStartupConfiguration(options.configurationLocations,
                                                    options.parseOptions);
        }

        auto packaged =
            config::loadPackagedConfiguration(options.configurationLocations, options.parseOptions);
        if (!packaged) {
            return Result<config::StartupConfiguration>::failure(std::move(packaged).error());
        }
        return Result<config::StartupConfiguration>::success({std::move(packaged).value(), {}});
    }();
    if (!startup) {
        return Result<std::unique_ptr<SettingsEngine>>::failure(std::move(startup).error());
    }

    auto engine = std::unique_ptr<SettingsEngine>(new SettingsEngine(
        std::move(options), std::make_shared<const theme::ThemeBundle>(std::move(bundle).value())));
    auto candidate = engine->buildCandidate(startup.value().candidate.configuration,
                                            startup.value().candidate.source, false,
                                            startupWarnings(startup.value().issues));
    if (!candidate) {
        return Result<std::unique_ptr<SettingsEngine>>::failure(std::move(candidate).error());
    }
    auto publication = engine->publications_.publish(std::move(candidate).value());
    if (!publication) {
        return Result<std::unique_ptr<SettingsEngine>>::failure(std::move(publication).error());
    }

    if (engine->options_.mode == SettingsEngineMode::normal &&
        startup.value().candidate.source == config::ConfigurationSource::user) {
        // Persistence follows the authoritative pointer swap. A failure must not imply rollback.
        (void)config::promoteLastKnownValidConfiguration(engine->options_.configurationLocations,
                                                         startup.value().candidate,
                                                         engine->options_.parseOptions);
    }
    return Result<std::unique_ptr<SettingsEngine>>::success(std::move(engine));
}

Result<SettingsCandidate>
SettingsEngine::buildCandidate(config::Configuration configuration,
                               config::ConfigurationSource source, bool runtimeProfileOverride,
                               std::vector<SettingsWarning> warnings) const {
    auto effectiveConfiguration = options_.mode == SettingsEngineMode::development_safe_mode
                                      ? config::withOptionalIntegrationsDisabled(configuration)
                                      : std::move(configuration);
    auto resolved = theme::resolveThemeCandidate(theme_bundle_->base, theme_bundle_->lustre,
                                                 theme_bundle_->forge, theme_bundle_->accessibility,
                                                 effectiveConfiguration, options_.themeOptions);
    if (!resolved) {
        return Result<SettingsCandidate>::failure(std::move(resolved).error());
    }
    const auto expectedProfile = effectiveConfiguration.profile == config::Profile::lustre
                                     ? theme::Profile::lustre
                                     : theme::Profile::forge;
    if (resolved.value().profile != expectedProfile) {
        return Result<SettingsCandidate>::failure(
            {ErrorCode::validation_error,
             "The resolved theme profile does not match the normalized configuration.",
             "Resolve one complete configuration and theme candidate together."});
    }
    return Result<SettingsCandidate>::success(SettingsCandidate{std::move(effectiveConfiguration),
                                                                {source, runtimeProfileOverride},
                                                                std::move(resolved).value(),
                                                                std::move(warnings)});
}

Result<PublicationOutcome> SettingsEngine::requestProfileChange(std::string_view profile) {
    config::Profile requested;
    if (profile == "lustre") {
        requested = config::Profile::lustre;
    } else if (profile == "forge") {
        requested = config::Profile::forge;
    } else {
        return Result<PublicationOutcome>::failure({ErrorCode::invalid_argument,
                                                    "The requested profile identifier is invalid.",
                                                    "Request exactly lustre or forge."});
    }

    if (current()->candidate.configuration.profile == requested) {
        return Result<PublicationOutcome>::success(PublicationOutcome{current(), {}, false, {}});
    }
    auto candidate = buildCandidate(
        config::withProfile(current()->candidate.configuration, requested),
        current()->candidate.provenance.configurationSource, true, current()->candidate.warnings);
    if (!candidate) {
        return Result<PublicationOutcome>::failure(std::move(candidate).error());
    }
    return publications_.publish(std::move(candidate).value());
}

Result<PublicationOutcome> SettingsEngine::reload() {
    auto reloaded = options_.mode == SettingsEngineMode::development_safe_mode
                        ? config::loadPackagedConfiguration(options_.configurationLocations,
                                                            options_.parseOptions)
                        : config::loadReloadConfiguration(options_.configurationLocations,
                                                          options_.parseOptions);
    if (!reloaded) {
        return Result<PublicationOutcome>::failure(std::move(reloaded).error());
    }
    auto bundle = theme::loadPackagedThemeBundle(options_.themeDirectory);
    if (!bundle) {
        return Result<PublicationOutcome>::failure(std::move(bundle).error());
    }
    const auto replacementBundle =
        std::make_shared<const theme::ThemeBundle>(std::move(bundle).value());
    const auto previousBundle = theme_bundle_;
    theme_bundle_ = replacementBundle;
    auto candidate =
        buildCandidate(reloaded.value().configuration, reloaded.value().source, false, {});
    if (!candidate) {
        theme_bundle_ = previousBundle;
        return Result<PublicationOutcome>::failure(std::move(candidate).error());
    }
    auto publication = publications_.publish(std::move(candidate).value());
    if (!publication) {
        theme_bundle_ = previousBundle;
        return publication;
    }
    if (options_.mode == SettingsEngineMode::normal && publication.value().published &&
        reloaded.value().source == config::ConfigurationSource::user) {
        const auto promotion = config::promoteLastKnownValidConfiguration(
            options_.configurationLocations, reloaded.value(), options_.parseOptions);
        if (!promotion) {
            // The swap is already authoritative. Report a stable operation warning without
            // consuming another generation or implying that the publication was rolled back.
            publication.value().operationWarnings.push_back(
                SettingsWarning::last_known_valid_persistence_failed);
        }
    }
    return publication;
}

Result<CandidateValidation>
SettingsEngine::validateCandidate(std::string_view candidateToml) const {
    if (candidateToml.size() > config::maximumConfigurationBytes) {
        return Result<CandidateValidation>::failure(
            {ErrorCode::too_large, "The candidate exceeds the 1 MiB validation limit.",
             "Submit a complete candidate no larger than 1 MiB."});
    }
    auto parsed = config::parseConfigurationToml(candidateToml, options_.parseOptions);
    if (!parsed) {
        return Result<CandidateValidation>::success(
            CandidateValidation{false, {invalidCandidateDiagnostic(parsed.error())}});
    }
    auto candidate =
        buildCandidate(std::move(parsed).value(), config::ConfigurationSource::user, false, {});
    if (!candidate) {
        return Result<CandidateValidation>::success(
            CandidateValidation{false, {invalidCandidateDiagnostic(candidate.error())}});
    }
    return Result<CandidateValidation>::success(CandidateValidation{true, {}});
}

} // namespace prismdrake::settings
