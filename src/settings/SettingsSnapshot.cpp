#include "SettingsSnapshot.hpp"

#include "RuntimeSnapshot.hpp"

#include <array>
#include <memory>
#include <utility>

namespace prismdrake::settings {
namespace {

using foundation::Result;

[[nodiscard]] std::vector<SettingsDomain> allDomains() {
    return {SettingsDomain::profile,     SettingsDomain::appearance,    SettingsDomain::panel,
            SettingsDomain::launcher,    SettingsDomain::notifications, SettingsDomain::desktop,
            SettingsDomain::integration, SettingsDomain::accessibility, SettingsDomain::keyboard,
            SettingsDomain::developer,   SettingsDomain::theme};
}

[[nodiscard]] std::vector<SettingsDomain> changedDomains(const SettingsCandidate &oldCandidate,
                                                         const SettingsCandidate &newCandidate) {
    std::vector<SettingsDomain> changed;
    const auto &oldConfig = oldCandidate.configuration;
    const auto &newConfig = newCandidate.configuration;
    if (oldConfig.profile != newConfig.profile) {
        changed.push_back(SettingsDomain::profile);
    }
    if (oldConfig.appearance != newConfig.appearance) {
        changed.push_back(SettingsDomain::appearance);
    }
    if (oldConfig.panel != newConfig.panel) {
        changed.push_back(SettingsDomain::panel);
    }
    if (oldConfig.launcher != newConfig.launcher) {
        changed.push_back(SettingsDomain::launcher);
    }
    if (oldConfig.notifications != newConfig.notifications) {
        changed.push_back(SettingsDomain::notifications);
    }
    if (oldConfig.desktop != newConfig.desktop) {
        changed.push_back(SettingsDomain::desktop);
    }
    if (oldConfig.integration != newConfig.integration) {
        changed.push_back(SettingsDomain::integration);
    }
    if (oldConfig.accessibility != newConfig.accessibility) {
        changed.push_back(SettingsDomain::accessibility);
    }
    if (oldConfig.keyboard != newConfig.keyboard) {
        changed.push_back(SettingsDomain::keyboard);
    }
    if (oldConfig.developer != newConfig.developer) {
        changed.push_back(SettingsDomain::developer);
    }
    if (oldCandidate.theme != newCandidate.theme) {
        changed.push_back(SettingsDomain::theme);
    }
    return changed;
}

} // namespace

std::string_view settingsDomainId(SettingsDomain domain) noexcept {
    constexpr std::array ids{"profile",       "appearance", "panel",       "launcher",
                             "notifications", "desktop",    "integration", "accessibility",
                             "keyboard",      "developer",  "theme"};
    return ids.at(static_cast<std::size_t>(domain));
}

std::string_view settingsWarningId(SettingsWarning warning) noexcept {
    constexpr std::array ids{"invalid_user_configuration", "invalid_last_known_valid_configuration",
                             "last_known_valid_persistence_failed"};
    return ids.at(static_cast<std::size_t>(warning));
}

std::string_view configurationSourceId(config::ConfigurationSource source) noexcept {
    constexpr std::array ids{"user", "last_known_valid", "packaged_default"};
    return ids.at(static_cast<std::size_t>(source));
}

std::string_view profileId(config::Profile profile) noexcept {
    constexpr std::array ids{"lustre", "forge"};
    return ids.at(static_cast<std::size_t>(profile));
}

std::string_view themeWarningId(theme::ThemeWarning warning) noexcept {
    constexpr std::array ids{"accent_suppressed_high_contrast", "blur_fallback_active",
                             "thumbnail_fallback_active", "safe_mode_active"};
    return ids.at(static_cast<std::size_t>(warning));
}

Result<PublicationOutcome> SettingsPublicationState::publish(SettingsCandidate candidate) {
    if (current_ && current_->candidate == candidate) {
        return Result<PublicationOutcome>::success(PublicationOutcome{current_, {}, false, {}});
    }

    const auto generation =
        current_
            ? current_->generation.next()
            : Result<foundation::Generation>::success(foundation::Generation::firstPublished());
    if (!generation) {
        return Result<PublicationOutcome>::failure(generation.error());
    }

    auto domains = current_ ? changedDomains(current_->candidate, candidate) : allDomains();
    auto serialized = serializeRuntimeSnapshot(generation.value(), candidate);
    if (!serialized) {
        return Result<PublicationOutcome>::failure(std::move(serialized).error());
    }
    auto next = std::make_shared<const SettingsSnapshot>(
        SettingsSnapshot{runtimeSnapshotSchemaVersion, generation.value(), std::move(candidate),
                         std::move(serialized).value().json});
    previous_ = current_;
    current_ = std::move(next);
    return Result<PublicationOutcome>::success(
        PublicationOutcome{current_, std::move(domains), true, {}});
}

} // namespace prismdrake::settings
