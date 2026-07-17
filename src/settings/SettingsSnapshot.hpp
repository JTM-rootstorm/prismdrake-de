#pragma once

#include "Configuration.hpp"
#include "ConfigurationLoader.hpp"
#include "Generation.hpp"
#include "Result.hpp"
#include "ThemeResolver.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace prismdrake::settings {

inline constexpr std::uint32_t runtimeSnapshotSchemaVersion = 1U;

enum class SettingsDomain : std::uint8_t {
    profile,
    appearance,
    panel,
    launcher,
    notifications,
    desktop,
    integration,
    accessibility,
    keyboard,
    developer,
    theme,
};

enum class SettingsWarning : std::uint8_t {
    invalid_user_configuration,
    invalid_last_known_valid_configuration,
    last_known_valid_persistence_failed,
};

struct SettingsProvenance final {
    config::ConfigurationSource configurationSource;
    bool runtimeProfileOverride;

    friend bool operator==(const SettingsProvenance &, const SettingsProvenance &) = default;
};

struct SettingsCandidate final {
    config::Configuration configuration;
    SettingsProvenance provenance;
    theme::ResolvedThemeCandidate theme;
    std::vector<SettingsWarning> warnings;

    friend bool operator==(const SettingsCandidate &, const SettingsCandidate &) = default;
};

/// One complete immutable settings/theme publication within one service-owner epoch.
struct SettingsSnapshot final {
    const std::uint32_t snapshotSchemaVersion;
    const foundation::Generation generation;
    const SettingsCandidate candidate;
};

struct PublicationOutcome final {
    std::shared_ptr<const SettingsSnapshot> snapshot;
    std::vector<SettingsDomain> changedDomains;
    bool published;
    std::vector<SettingsWarning> operationWarnings;
};

[[nodiscard]] std::string_view settingsDomainId(SettingsDomain domain) noexcept;
[[nodiscard]] std::string_view settingsWarningId(SettingsWarning warning) noexcept;
[[nodiscard]] std::string_view configurationSourceId(config::ConfigurationSource source) noexcept;
[[nodiscard]] std::string_view profileId(config::Profile profile) noexcept;
[[nodiscard]] std::string_view themeWarningId(theme::ThemeWarning warning) noexcept;

/// Single-owner publication state. Captured snapshot pointers preserve immutable lifetime.
class SettingsPublicationState final {
  public:
    [[nodiscard]] foundation::Result<PublicationOutcome> publish(SettingsCandidate candidate);

    [[nodiscard]] const std::shared_ptr<const SettingsSnapshot> &current() const noexcept {
        return current_;
    }
    [[nodiscard]] const std::shared_ptr<const SettingsSnapshot> &previous() const noexcept {
        return previous_;
    }

  private:
    std::shared_ptr<const SettingsSnapshot> current_;
    std::shared_ptr<const SettingsSnapshot> previous_;
};

} // namespace prismdrake::settings
