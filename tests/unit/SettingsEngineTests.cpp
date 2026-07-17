#include "RuntimeSnapshot.hpp"
#include "SettingsEngine.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace prismdrake::settings {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint counter{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-settings-test-" + std::to_string(counter.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(stream);
}

[[nodiscard]] SettingsEngineOptions optionsFor(const TemporaryDirectory &temporary) {
    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    return {{temporary.path() / "config/config.toml",
             temporary.path() / "state/last-known-valid-config.toml",
             source / "data/defaults/config.toml"},
            source / "themes",
            {},
            {}};
}

[[nodiscard]] std::string defaultConfiguration() {
    std::ifstream stream(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                         "data/defaults/config.toml");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::string fixtureConfiguration(std::string_view relativePath) {
    std::ifstream stream(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) / relativePath);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::set<std::string> objectKeys(const nlohmann::json &object) {
    std::set<std::string> keys;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        keys.insert(iterator.key());
    }
    return keys;
}

TEST(SettingsEngineTest, PublishesOneCompleteInitialGeneration) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));

    ASSERT_TRUE(engine);
    ASSERT_TRUE(engine.value()->current());
    EXPECT_EQ(engine.value()->current()->generation.value(), 1U);
    EXPECT_EQ(engine.value()->current()->snapshotSchemaVersion, runtimeSnapshotSchemaVersion);
    EXPECT_EQ(engine.value()->current()->candidate.configuration.profile, config::Profile::lustre);
    EXPECT_EQ(engine.value()->current()->candidate.theme.profile, theme::Profile::lustre);
    EXPECT_FALSE(engine.value()->previous());
}

TEST(SettingsEngineTest, ChangesProfileAtomicallyAndKeepsPreviousSnapshotAlive) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);
    const auto first = engine.value()->current();

    auto changed = engine.value()->requestProfileChange("forge");

    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed.value().published);
    EXPECT_EQ(changed.value().snapshot->generation.value(), 2U);
    EXPECT_EQ(changed.value().snapshot->candidate.configuration.profile, config::Profile::forge);
    EXPECT_EQ(changed.value().snapshot->candidate.theme.profile, theme::Profile::forge);
    EXPECT_EQ(changed.value().snapshot->candidate.configuration.accessibility,
              first->candidate.configuration.accessibility);
    ASSERT_TRUE(engine.value()->previous());
    EXPECT_EQ(engine.value()->previous()->generation.value(), 1U);
    EXPECT_EQ(first->generation.value(), 1U);
    EXPECT_EQ(changed.value().changedDomains,
              (std::vector{SettingsDomain::profile, SettingsDomain::theme}));

    const auto same = engine.value()->requestProfileChange("forge");
    ASSERT_TRUE(same);
    EXPECT_FALSE(same.value().published);
    EXPECT_EQ(same.value().snapshot->generation.value(), 2U);
}

TEST(SettingsEngineTest, InvalidProfileAndReloadPreserveTheAuthoritativeGeneration) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);

    const auto invalidProfile = engine.value()->requestProfileChange("Lustre");
    ASSERT_FALSE(invalidProfile);
    EXPECT_EQ(invalidProfile.error().code, foundation::ErrorCode::invalid_argument);
    EXPECT_EQ(engine.value()->current()->generation.value(), 1U);

    writeText(optionsFor(temporary).configurationLocations.user,
              "schema_version = 1\nprivate_secret = \"do-not-echo\"\n");
    const auto invalidReload = engine.value()->reload();
    ASSERT_FALSE(invalidReload);
    EXPECT_EQ(engine.value()->current()->generation.value(), 1U);
}

TEST(SettingsEngineTest, ReloadClearsTheRuntimeProfileOverride) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);
    ASSERT_TRUE(engine.value()->requestProfileChange("forge"));

    const auto reloaded = engine.value()->reload();

    ASSERT_TRUE(reloaded);
    EXPECT_TRUE(reloaded.value().published);
    EXPECT_EQ(reloaded.value().snapshot->generation.value(), 3U);
    EXPECT_EQ(reloaded.value().snapshot->candidate.configuration.profile, config::Profile::lustre);
    EXPECT_FALSE(reloaded.value().snapshot->candidate.provenance.runtimeProfileOverride);
}

TEST(SettingsEngineTest, CandidateValidationIsBoundedRedactedAndSideEffectFree) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);

    const auto valid = engine.value()->validateCandidate(defaultConfiguration());
    ASSERT_TRUE(valid);
    EXPECT_TRUE(valid.value().valid);
    EXPECT_TRUE(valid.value().diagnostics.empty());

    const auto invalid = engine.value()->validateCandidate(
        "schema_version = 1\nprivate_secret = \"never-return-this\"\n");
    ASSERT_TRUE(invalid);
    ASSERT_FALSE(invalid.value().valid);
    ASSERT_EQ(invalid.value().diagnostics.size(), 1U);
    const auto &diagnostic = invalid.value().diagnostics.front();
    EXPECT_EQ(diagnostic.fieldPath, "$");
    EXPECT_EQ(diagnostic.diagnosticCode, "invalid_configuration");
    EXPECT_EQ(diagnostic.logicalSourceId, "candidate");

    const std::string oversized(config::maximumConfigurationBytes + 1U, 'x');
    const auto tooLarge = engine.value()->validateCandidate(oversized);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, foundation::ErrorCode::too_large);
    EXPECT_EQ(engine.value()->current()->generation.value(), 1U);
}

TEST(SettingsEngineTest, SafeModeUsesOnlyPackagedDefaultsAndDisablesOptionalIntegrations) {
    TemporaryDirectory temporary;
    auto options = optionsFor(temporary);
    const auto userDocument = fixtureConfiguration("examples/config/forge.toml");
    const auto recoveryDocument = fixtureConfiguration("examples/config/lustre.toml");
    writeText(options.configurationLocations.user, userDocument);
    writeText(options.configurationLocations.lastKnownValid, recoveryDocument);
    options.mode = SettingsEngineMode::development_safe_mode;

    auto engine = SettingsEngine::start(options);

    ASSERT_TRUE(engine);
    const auto &snapshot = engine.value()->current();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->candidate.provenance.configurationSource,
              config::ConfigurationSource::packaged_default);
    EXPECT_EQ(snapshot->candidate.configuration.profile, config::Profile::lustre);
    const auto &integration = snapshot->candidate.configuration.integration;
    EXPECT_FALSE(integration.exportGtk);
    EXPECT_FALSE(integration.exportQt);
    EXPECT_FALSE(integration.exportXsettings);
    EXPECT_FALSE(integration.exportPortal);
    EXPECT_TRUE(snapshot->candidate.theme.accessibility.reducedMotion);
    EXPECT_TRUE(snapshot->candidate.theme.accessibility.transparencyDisabled);
    EXPECT_EQ(snapshot->candidate.theme.accessibility.animationScale, 0.0);
    EXPECT_EQ(snapshot->candidate.theme.materials.panel.opacity, 1.0);
    EXPECT_TRUE(snapshot->candidate.theme.materials.panel.usedFallback);
    EXPECT_NE(std::find(snapshot->candidate.theme.warnings.begin(),
                        snapshot->candidate.theme.warnings.end(),
                        theme::ThemeWarning::safe_mode_active),
              snapshot->candidate.theme.warnings.end());

    ASSERT_TRUE(engine.value()->requestProfileChange("forge"));
    const auto reloaded = engine.value()->reload();
    ASSERT_TRUE(reloaded);
    EXPECT_TRUE(reloaded.value().published);
    EXPECT_EQ(reloaded.value().snapshot->candidate.configuration.profile, config::Profile::lustre);

    std::ifstream recovery(options.configurationLocations.lastKnownValid, std::ios::binary);
    const std::string retained{std::istreambuf_iterator<char>(recovery),
                               std::istreambuf_iterator<char>()};
    EXPECT_EQ(retained, recoveryDocument);
    std::ifstream user(options.configurationLocations.user, std::ios::binary);
    const std::string retainedUser{std::istreambuf_iterator<char>(user),
                                   std::istreambuf_iterator<char>()};
    EXPECT_EQ(retainedUser, userDocument);
}

TEST(SettingsEngineTest, SafeModeFailsIfThePackagedDefaultIsInvalid) {
    TemporaryDirectory temporary;
    auto options = optionsFor(temporary);
    writeText(options.configurationLocations.user,
              fixtureConfiguration("examples/config/forge.toml"));
    writeText(options.configurationLocations.lastKnownValid,
              fixtureConfiguration("examples/config/lustre.toml"));
    options.configurationLocations.packagedDefault = temporary.path() / "packaged/config.toml";
    writeText(options.configurationLocations.packagedDefault, "schema_version = 99\n");
    options.mode = SettingsEngineMode::development_safe_mode;

    const auto engine = SettingsEngine::start(options);

    ASSERT_FALSE(engine);
    EXPECT_EQ(engine.error().code, foundation::ErrorCode::unsupported);
}

TEST(SettingsSnapshotTest, StableIdentifiersCoverClosedWireValues) {
    EXPECT_EQ(settingsDomainId(SettingsDomain::accessibility), "accessibility");
    EXPECT_EQ(settingsWarningId(SettingsWarning::invalid_user_configuration),
              "invalid_user_configuration");
    EXPECT_EQ(configurationSourceId(config::ConfigurationSource::packaged_default),
              "packaged_default");
    EXPECT_EQ(profileId(config::Profile::forge), "forge");
    EXPECT_EQ(themeWarningId(theme::ThemeWarning::safe_mode_active), "safe_mode_active");
}

TEST(SettingsSnapshotTest, SerializesOneBoundedCompleteGenerationWithoutFilesystemPaths) {
    TemporaryDirectory temporary;
    constexpr std::string_view privateValue = "runtime-snapshot-private-value";
    writeText(optionsFor(temporary).configurationLocations.user,
              "schema_version = 1\nprivate_secret = \"runtime-snapshot-private-value\"\n");
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);

    const auto &serialized = engine.value()->current()->serializedJson;

    EXPECT_LE(serialized.size(), maximumRuntimeSnapshotBytes);
    const auto document = nlohmann::json::parse(serialized);
    EXPECT_EQ(document.at("schema_version"), runtimeSnapshotSchemaVersion);
    EXPECT_EQ(document.at("generation"), engine.value()->current()->generation.value());
    EXPECT_EQ(document.at("profile_id"), "lustre");
    EXPECT_EQ(
        objectKeys(document),
        (std::set<std::string>{"configuration_source_id", "generation", "profile_id",
                               "restart_required_domains", "runtime_profile_override",
                               "schema_version", "settings", "theme", "validation_warning_ids"}));
    EXPECT_EQ(objectKeys(document.at("settings")),
              (std::set<std::string>{"accessibility", "appearance", "desktop", "developer",
                                     "integration", "keyboard", "launcher", "notifications",
                                     "panel", "schema_version"}));
    EXPECT_FALSE(document.at("settings").contains("profile"));
    EXPECT_EQ(document.at("settings").at("appearance").at("accent"), "#4D7FFF");
    EXPECT_EQ(document.at("theme").at("profile_id"), "lustre");
    EXPECT_EQ(document.at("theme").at("profile_display_name"), "Prismdrake Lustre");
    EXPECT_EQ(document.at("theme").at("logical_source_ids"),
              nlohmann::json::array({"packaged_base", "packaged_lustre"}));
    EXPECT_TRUE(document.at("theme").contains("primitive"));
    EXPECT_TRUE(document.at("theme").contains("semantic"));
    EXPECT_TRUE(document.at("theme").contains("component"));
    EXPECT_EQ(serialized.find(temporary.path().string()), std::string::npos);
    EXPECT_EQ(serialized.find(privateValue), std::string::npos);
    EXPECT_EQ(serialized.find("private_secret"), std::string::npos);
    EXPECT_EQ(document.at("validation_warning_ids"),
              nlohmann::json::array({"invalid_user_configuration"}));
}

TEST(SettingsSnapshotTest, SerializationFailureDoesNotPublishOrConsumeAGeneration) {
    TemporaryDirectory temporary;
    auto engine = SettingsEngine::start(optionsFor(temporary));
    ASSERT_TRUE(engine);

    SettingsPublicationState state;
    const auto initial = state.publish(engine.value()->current()->candidate);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(state.current());
    EXPECT_EQ(state.current()->generation.value(), 1U);

    const auto &configuration = engine.value()->current()->candidate.configuration;
    const config::Configuration oversizedConfiguration{
        configuration.schemaVersion,
        configuration.profile,
        configuration.appearance,
        configuration.panel,
        configuration.launcher,
        configuration.notifications,
        configuration.desktop,
        configuration.integration,
        configuration.accessibility,
        configuration.keyboard,
        config::Developer{false, {std::string(maximumRuntimeSnapshotBytes + 1U, 'x')}}};
    SettingsCandidate oversizedCandidate{
        oversizedConfiguration, engine.value()->current()->candidate.provenance,
        engine.value()->current()->candidate.theme, engine.value()->current()->candidate.warnings};

    const auto rejected = state.publish(std::move(oversizedCandidate));

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::too_large);
    ASSERT_TRUE(state.current());
    EXPECT_EQ(state.current()->generation.value(), 1U);
    EXPECT_FALSE(state.previous());
}

} // namespace
} // namespace prismdrake::settings
