#include "SettingsEngine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
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

TEST(SettingsSnapshotTest, StableIdentifiersCoverClosedWireValues) {
    EXPECT_EQ(settingsDomainId(SettingsDomain::accessibility), "accessibility");
    EXPECT_EQ(settingsWarningId(SettingsWarning::invalid_user_configuration),
              "invalid_user_configuration");
    EXPECT_EQ(configurationSourceId(config::ConfigurationSource::packaged_default),
              "packaged_default");
    EXPECT_EQ(profileId(config::Profile::forge), "forge");
    EXPECT_EQ(themeWarningId(theme::ThemeWarning::safe_mode_active), "safe_mode_active");
}

} // namespace
} // namespace prismdrake::settings
