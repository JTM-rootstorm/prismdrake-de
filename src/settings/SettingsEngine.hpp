#pragma once

#include "ConfigurationLoader.hpp"
#include "Result.hpp"
#include "SettingsSnapshot.hpp"
#include "ThemeBundle.hpp"
#include "ThemeResolver.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::settings {

inline constexpr std::size_t maximumValidationDiagnostics = 64U;

enum class SettingsEngineMode : std::uint8_t {
    normal,
    development_safe_mode,
};

struct ValidationDiagnostic final {
    std::string logicalSourceId;
    std::string fieldPath;
    std::string diagnosticCode;
    std::string expectedId;
    std::string recoveryId;

    friend bool operator==(const ValidationDiagnostic &, const ValidationDiagnostic &) = default;
};

struct CandidateValidation final {
    bool valid;
    std::vector<ValidationDiagnostic> diagnostics;
};

struct SettingsEngineOptions final {
    config::ConfigurationLocations configurationLocations;
    std::filesystem::path themeDirectory;
    config::ConfigurationParseOptions parseOptions;
    theme::ThemeResolveOptions themeOptions;
    SettingsEngineMode mode = SettingsEngineMode::normal;
};

/// Display-free owner of complete settings/theme transactions for one service epoch.
class SettingsEngine final {
  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<SettingsEngine>>
    start(SettingsEngineOptions options);

    [[nodiscard]] const std::shared_ptr<const SettingsSnapshot> &current() const noexcept {
        return publications_.current();
    }
    [[nodiscard]] const std::shared_ptr<const SettingsSnapshot> &previous() const noexcept {
        return publications_.previous();
    }

    [[nodiscard]] foundation::Result<PublicationOutcome>
    requestProfileChange(std::string_view profile);
    [[nodiscard]] foundation::Result<PublicationOutcome> reload();
    [[nodiscard]] foundation::Result<CandidateValidation>
    validateCandidate(std::string_view candidateToml) const;

  private:
    SettingsEngine(SettingsEngineOptions options, std::shared_ptr<const theme::ThemeBundle> bundle)
        : options_(std::move(options)), theme_bundle_(std::move(bundle)) {}

    [[nodiscard]] foundation::Result<SettingsCandidate>
    buildCandidate(config::Configuration configuration, config::ConfigurationSource source,
                   bool runtimeProfileOverride, std::vector<SettingsWarning> warnings) const;

    SettingsEngineOptions options_;
    std::shared_ptr<const theme::ThemeBundle> theme_bundle_;
    SettingsPublicationState publications_;
};

} // namespace prismdrake::settings
