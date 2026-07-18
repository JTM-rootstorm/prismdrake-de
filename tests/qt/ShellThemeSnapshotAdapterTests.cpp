#include "ShellThemeSnapshotAdapter.hpp"

#include "SettingsEngine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace prismdrake::shell::theme {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint counter{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-shell-theme-test-" + std::to_string(counter.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string readFixture(std::string_view relativePath) {
    std::ifstream stream(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) / relativePath,
                         std::ios::binary);
    if (!stream) {
        throw std::runtime_error("unable to read shell theme test fixture");
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("unable to create shell theme test configuration");
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("unable to write shell theme test configuration");
    }
}

[[nodiscard]] std::string replaceOnce(std::string input, std::string_view before,
                                      std::string_view after) {
    const auto position = input.find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("shell theme test replacement target is absent");
    }
    input.replace(position, before.size(), after);
    return input;
}

class EngineFixture final {
  public:
    explicit EngineFixture(std::string configuration,
                           prismdrake::theme::ThemeResolveOptions themeOptions = {}) {
        const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
        const config::ConfigurationLocations locations{temporary_.path() / "config/config.toml",
                                                       temporary_.path() /
                                                           "state/last-known-valid-config.toml",
                                                       source / "data/defaults/config.toml"};
        writeText(locations.user, configuration);
        auto started =
            settings::SettingsEngine::start({locations, source / "themes", {}, themeOptions});
        if (!started) {
            throw std::runtime_error(started.error().message);
        }
        engine_ = std::move(started).value();
    }

    [[nodiscard]] settings::SettingsEngine &engine() noexcept { return *engine_; }

  private:
    TemporaryDirectory temporary_;
    std::unique_ptr<settings::SettingsEngine> engine_;
};

[[nodiscard]] std::shared_ptr<const settings::SettingsSnapshot>
snapshotWithSemantic(const settings::SettingsSnapshot &source,
                     prismdrake::theme::SemanticTokens semantic,
                     std::uint32_t schemaVersion = settings::runtimeSnapshotSchemaVersion) {
    const auto &resolved = source.candidate.theme;
    prismdrake::theme::ResolvedThemeCandidate replacementTheme{
        resolved.schemaVersion, resolved.profile,       resolved.profileDisplayName,
        resolved.sources,       resolved.primitive,     std::move(semantic),
        resolved.component,     resolved.accessibility, resolved.capabilityFallbacks,
        resolved.materials,     resolved.thumbnails,    resolved.warnings};
    settings::SettingsCandidate candidate{source.candidate.configuration,
                                          source.candidate.provenance, std::move(replacementTheme),
                                          source.candidate.warnings};
    return std::make_shared<const settings::SettingsSnapshot>(settings::SettingsSnapshot{
        schemaVersion, source.generation, std::move(candidate), source.serializedJson});
}

[[nodiscard]] std::shared_ptr<const settings::SettingsSnapshot>
snapshotWithConfiguration(const settings::SettingsSnapshot &source,
                          config::Configuration configuration,
                          std::uint32_t schemaVersion = settings::runtimeSnapshotSchemaVersion) {
    settings::SettingsCandidate candidate{std::move(configuration), source.candidate.provenance,
                                          source.candidate.theme, source.candidate.warnings};
    return std::make_shared<const settings::SettingsSnapshot>(settings::SettingsSnapshot{
        schemaVersion, source.generation, std::move(candidate), source.serializedJson});
}

TEST(ShellThemeSnapshotAdapterTest, ProjectsRealLustreAndForgeSettingsSnapshots) {
    constexpr prismdrake::theme::ThemeResolveOptions capabilities{{true, true}, false};
    EngineFixture lustre(readFixture("examples/config/lustre.toml"), capabilities);
    EngineFixture forge(readFixture("examples/config/forge.toml"), capabilities);
    ShellThemeSnapshotAdapter lustreAdapter;
    ShellThemeSnapshotAdapter forgeAdapter;

    ASSERT_TRUE(lustreAdapter.applySnapshot(lustre.engine().current()));
    ASSERT_TRUE(forgeAdapter.applySnapshot(forge.engine().current()));

    const auto *lustreGeneration = lustreAdapter.current();
    ASSERT_NE(lustreGeneration, nullptr);
    EXPECT_EQ(lustreGeneration->generation().value(), 1U);
    EXPECT_EQ(lustreGeneration->generationId(), QStringLiteral("1"));
    EXPECT_EQ(lustreGeneration->profileId(), QStringLiteral("lustre"));
    EXPECT_EQ(lustreGeneration->profileDisplayName(), QStringLiteral("Prismdrake Lustre"));
    EXPECT_FALSE(lustreGeneration->panel()->fallbackActive());
    EXPECT_TRUE(lustreGeneration->panel()->blurRequested());
    EXPECT_EQ(lustreGeneration->panel()->surfaceColor().red(), 0x20);
    EXPECT_EQ(lustreGeneration->panel()->surfaceColor().green(), 0x2a);
    EXPECT_EQ(lustreGeneration->panel()->surfaceColor().blue(), 0x42);
    EXPECT_NEAR(lustreGeneration->panel()->surfaceColor().alphaF(), 0.82, 0.0001);
    EXPECT_NEAR(lustreGeneration->notification()->surfaceColor().alphaF(), 0.9, 0.0001);
    EXPECT_NEAR(lustreGeneration->launcher()->surfaceColor().alphaF(), 0.86, 0.0001);
    EXPECT_TRUE(lustreGeneration->launcher()->blurRequested());
    EXPECT_FALSE(lustreGeneration->launcher()->fallbackActive());
    EXPECT_DOUBLE_EQ(lustreGeneration->launcher()->tileRadius(), 11.0);
    EXPECT_DOUBLE_EQ(lustreGeneration->panel()->bodyFontPixels(), 14.0);
    EXPECT_DOUBLE_EQ(lustreGeneration->panel()->titleFontPixels(), 17.0);
    EXPECT_DOUBLE_EQ(lustreGeneration->panel()->panelHeight(), 48.0);
    EXPECT_DOUBLE_EQ(lustreGeneration->panel()->launcherRadius(), 11.0);
    EXPECT_DOUBLE_EQ(lustreGeneration->notification()->cardRadius(), 12.0);

    const auto *forgeGeneration = forgeAdapter.current();
    ASSERT_NE(forgeGeneration, nullptr);
    EXPECT_EQ(forgeGeneration->profileId(), QStringLiteral("forge"));
    EXPECT_TRUE(forgeGeneration->transparencyDisabled());
    EXPECT_TRUE(forgeGeneration->panel()->fallbackActive());
    EXPECT_TRUE(forgeGeneration->launcher()->fallbackActive());
    EXPECT_FALSE(forgeGeneration->launcher()->blurRequested());
    EXPECT_DOUBLE_EQ(forgeGeneration->launcher()->tileBorderWidth(), 2.0);
    EXPECT_FALSE(forgeGeneration->panel()->blurRequested());
    EXPECT_EQ(forgeGeneration->panel()->surfaceColor(), QColor::fromRgb(0x34, 0x30, 0x28));
    EXPECT_DOUBLE_EQ(forgeGeneration->panel()->panelHeight(), 44.0);
    EXPECT_DOUBLE_EQ(forgeGeneration->panel()->taskBorderWidth(), 2.0);
    EXPECT_EQ(forgeGeneration->panel()->fastMotionMs(), 45);
}

TEST(ShellThemeSnapshotAdapterTest, AppliesProfileSwitchAsOneCompleteGeneration) {
    constexpr prismdrake::theme::ThemeResolveOptions capabilities{{true, true}, false};
    EngineFixture fixture(readFixture("examples/config/lustre.toml"), capabilities);
    ShellThemeSnapshotAdapter adapter;
    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));
    auto changed = fixture.engine().requestProfileChange("forge");
    ASSERT_TRUE(changed);

    bool observedCoherentGeneration = false;
    QObject::connect(&adapter, &ShellThemeSnapshotAdapter::currentChanged, &adapter, [&]() {
        const auto *current = adapter.current();
        observedCoherentGeneration = current != nullptr && current->generation().value() == 2U &&
                                     current->profileId() == QStringLiteral("forge") &&
                                     current->panel()->surfaceColor().red() == 0x34 &&
                                     current->notification()->surfaceColor().red() == 0x49;
    });

    ASSERT_TRUE(adapter.applySnapshot(changed.value().snapshot));
    EXPECT_TRUE(observedCoherentGeneration);
    ASSERT_TRUE(adapter.previousGeneration());
    EXPECT_EQ(adapter.previousGeneration()->generation().value(), 1U);
    EXPECT_EQ(adapter.currentGeneration()->generation().value(), 2U);
}

TEST(ShellThemeSnapshotAdapterTest, ProjectsAccessibilityFallbackAndFractionalTextExactly) {
    constexpr prismdrake::theme::ThemeResolveOptions capabilities{{true, true}, false};
    EngineFixture accessible(readFixture("examples/config/accessible.toml"), capabilities);
    auto fractionalConfiguration = replaceOnce(readFixture("examples/config/lustre.toml"),
                                               "text_scale = 1.0", "text_scale = 1.25");
    EngineFixture fractional(std::move(fractionalConfiguration), capabilities);
    ShellThemeSnapshotAdapter accessibleAdapter;
    ShellThemeSnapshotAdapter fractionalAdapter;

    ASSERT_TRUE(accessibleAdapter.applySnapshot(accessible.engine().current()));
    ASSERT_TRUE(fractionalAdapter.applySnapshot(fractional.engine().current()));

    const auto *accessibleGeneration = accessibleAdapter.current();
    ASSERT_NE(accessibleGeneration, nullptr);
    EXPECT_TRUE(accessibleGeneration->highContrast());
    EXPECT_TRUE(accessibleGeneration->reducedMotion());
    EXPECT_TRUE(accessibleGeneration->transparencyDisabled());
    EXPECT_DOUBLE_EQ(accessibleGeneration->textScale(), 1.5);
    EXPECT_DOUBLE_EQ(accessibleGeneration->panel()->bodyFontPixels(), 21.0);
    EXPECT_DOUBLE_EQ(accessibleGeneration->panel()->titleFontPixels(), 25.5);
    EXPECT_DOUBLE_EQ(accessibleGeneration->panel()->focusWidth(), 4.0);
    EXPECT_DOUBLE_EQ(accessibleGeneration->panel()->minimumTargetSize(), 48.0);
    EXPECT_DOUBLE_EQ(accessibleGeneration->panel()->panelHeight(), 64.0);
    EXPECT_EQ(accessibleGeneration->notification()->fastMotionMs(), 0);
    EXPECT_TRUE(accessibleGeneration->notification()->fallbackActive());
    EXPECT_EQ(accessibleGeneration->notification()->surfaceColor().alpha(), 255);
    EXPECT_TRUE(accessibleGeneration->launcher()->fallbackActive());
    EXPECT_TRUE(accessibleGeneration->launcher()->reducedMotion());
    EXPECT_EQ(accessibleGeneration->launcher()->fastMotionMs(), 0);
    EXPECT_DOUBLE_EQ(accessibleGeneration->launcher()->minimumTargetSize(), 48.0);

    const auto *fractionalGeneration = fractionalAdapter.current();
    ASSERT_NE(fractionalGeneration, nullptr);
    EXPECT_DOUBLE_EQ(fractionalGeneration->textScale(), 1.25);
    EXPECT_DOUBLE_EQ(fractionalGeneration->panel()->bodyFontPixels(), 17.5);
    EXPECT_DOUBLE_EQ(fractionalGeneration->notification()->bodyFontPixels(), 17.5);
    EXPECT_DOUBLE_EQ(fractionalGeneration->notification()->titleFontPixels(), 21.25);
}

TEST(ShellThemeSnapshotAdapterTest, DistinguishesMissingBlurFromTransparencyPreference) {
    constexpr prismdrake::theme::ThemeResolveOptions missingBlur{{false, false}, false};
    EngineFixture fixture(readFixture("examples/config/lustre.toml"), missingBlur);
    ShellThemeSnapshotAdapter adapter;

    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));

    ASSERT_NE(adapter.current(), nullptr);
    EXPECT_FALSE(adapter.current()->transparencyDisabled());
    EXPECT_TRUE(adapter.current()->panel()->fallbackActive());
    EXPECT_FALSE(adapter.current()->panel()->blurRequested());
    EXPECT_EQ(adapter.current()->panel()->surfaceColor().alpha(), 255);
    EXPECT_TRUE(adapter.current()->notification()->fallbackActive());
    EXPECT_TRUE(adapter.current()->launcher()->fallbackActive());
    EXPECT_FALSE(adapter.current()->launcher()->blurRequested());
    EXPECT_EQ(adapter.current()->launcher()->surfaceColor().alpha(), 255);
}

TEST(ShellThemeSnapshotAdapterTest, RejectsAbsentSchemaMismatchProfileMismatchAndConflict) {
    EngineFixture fixture(readFixture("examples/config/lustre.toml"));
    ShellThemeSnapshotAdapter adapter;
    const auto valid = fixture.engine().current();
    ASSERT_TRUE(adapter.applySnapshot(valid));
    const auto retained = adapter.currentGeneration();

    const auto absent = adapter.applySnapshot(nullptr);
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, foundation::ErrorCode::invalid_argument);

    const auto &semantic = valid->candidate.theme.semantic;
    auto wrongSchema = snapshotWithSemantic(*valid, semantic, 99U);
    const auto schemaRejected = adapter.applySnapshot(std::move(wrongSchema));
    ASSERT_FALSE(schemaRejected);
    EXPECT_EQ(schemaRejected.error().code, foundation::ErrorCode::validation_error);

    auto mismatched = snapshotWithConfiguration(
        *valid, config::withProfile(valid->candidate.configuration, config::Profile::forge));
    const auto profileRejected = adapter.applySnapshot(std::move(mismatched));
    ASSERT_FALSE(profileRejected);
    EXPECT_EQ(profileRejected.error().code, foundation::ErrorCode::validation_error);

    auto conflict = std::make_shared<const settings::SettingsSnapshot>(settings::SettingsSnapshot{
        valid->snapshotSchemaVersion, valid->generation, valid->candidate, valid->serializedJson});
    const auto conflictRejected = adapter.applySnapshot(std::move(conflict));
    ASSERT_FALSE(conflictRejected);
    EXPECT_EQ(conflictRejected.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(adapter.currentGeneration(), retained);
}

TEST(ShellThemeSnapshotAdapterTest, RejectsNonFiniteAndUnmappableUsedTokens) {
    EngineFixture fixture(readFixture("examples/config/lustre.toml"));
    ShellThemeSnapshotAdapter adapter;
    const auto valid = fixture.engine().current();
    ASSERT_TRUE(adapter.applySnapshot(valid));
    const auto retained = adapter.currentGeneration();
    const auto &semantic = valid->candidate.theme.semantic;

    const prismdrake::theme::Typography invalidTypography{
        semantic.typography.bodyFamily, std::numeric_limits<double>::quiet_NaN(),
        semantic.typography.titleSizePx, semantic.typography.weightNormal,
        semantic.typography.weightEmphasis};
    prismdrake::theme::SemanticTokens nonFiniteSemantic{
        semantic.colors,   semantic.materials, semantic.border, semantic.shadow,
        invalidTypography, semantic.spacing,   semantic.panel,  semantic.decoration,
        semantic.icon,     semantic.focus,     semantic.motion, semantic.targets};
    const auto nonFinite = adapter.applySnapshot(snapshotWithSemantic(*valid, nonFiniteSemantic));
    ASSERT_FALSE(nonFinite);
    EXPECT_EQ(nonFinite.error().code, foundation::ErrorCode::validation_error);

    auto missingPanelToken = semantic.panel;
    missingPanelToken.erase("height_px");
    prismdrake::theme::SemanticTokens unmappableSemantic{semantic.colors,
                                                         semantic.materials,
                                                         semantic.border,
                                                         semantic.shadow,
                                                         semantic.typography,
                                                         semantic.spacing,
                                                         std::move(missingPanelToken),
                                                         semantic.decoration,
                                                         semantic.icon,
                                                         semantic.focus,
                                                         semantic.motion,
                                                         semantic.targets};
    const auto unmappable =
        adapter.applySnapshot(snapshotWithSemantic(*valid, std::move(unmappableSemantic)));
    ASSERT_FALSE(unmappable);
    EXPECT_EQ(unmappable.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(adapter.currentGeneration(), retained);
}

TEST(ShellThemeSnapshotAdapterTest, RejectsStaleAndCrossThreadCallsWithoutMutation) {
    EngineFixture fixture(readFixture("examples/config/lustre.toml"));
    ShellThemeSnapshotAdapter adapter;
    const auto first = fixture.engine().current();
    ASSERT_TRUE(adapter.applySnapshot(first));
    auto changed = fixture.engine().requestProfileChange("forge");
    ASSERT_TRUE(changed);
    ASSERT_TRUE(adapter.applySnapshot(changed.value().snapshot));
    const auto retained = adapter.currentGeneration();

    const auto stale = adapter.applySnapshot(first);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, foundation::ErrorCode::cancelled);

    std::promise<foundation::Result<void>> promise;
    auto future = promise.get_future();
    std::thread worker([&]() { promise.set_value(adapter.applySnapshot(first)); });
    worker.join();
    const auto crossThread = future.get();
    ASSERT_FALSE(crossThread);
    EXPECT_EQ(crossThread.error().code, foundation::ErrorCode::cancelled);
    EXPECT_EQ(adapter.currentGeneration(), retained);
}

TEST(ShellThemeSnapshotAdapterTest, CapturedPriorGenerationRetainsItsCompleteObjectGraph) {
    EngineFixture fixture(readFixture("examples/config/lustre.toml"));
    ShellThemeSnapshotAdapter adapter;
    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));
    auto first = adapter.currentGeneration();
    const std::weak_ptr<const ShellThemeGeneration> firstWeak = first;
    ASSERT_TRUE(fixture.engine().requestProfileChange("forge"));
    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));
    ASSERT_TRUE(fixture.engine().requestProfileChange("lustre"));
    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));

    ASSERT_TRUE(first);
    EXPECT_EQ(first->generation().value(), 1U);
    EXPECT_EQ(first->profileId(), QStringLiteral("lustre"));
    EXPECT_EQ(first->panel()->surfaceColor().red(), 0x20);
    EXPECT_EQ(first->notification()->surfaceColor().red(), 0x2c);
    EXPECT_EQ(first->launcher()->surfaceColor().red(), 0x26);
    EXPECT_FALSE(firstWeak.expired());

    first.reset();
    EXPECT_TRUE(firstWeak.expired());
}

TEST(ShellThemeSnapshotAdapterTest, RejectsReentrantPublicationWithCoherentOuterGeneration) {
    EngineFixture fixture(readFixture("examples/config/lustre.toml"));
    ShellThemeSnapshotAdapter adapter;
    ASSERT_TRUE(adapter.applySnapshot(fixture.engine().current()));
    auto second = fixture.engine().requestProfileChange("forge");
    ASSERT_TRUE(second);
    auto third = fixture.engine().requestProfileChange("lustre");
    ASSERT_TRUE(third);
    std::optional<foundation::Result<void>> reentrant;

    QObject::connect(&adapter, &ShellThemeSnapshotAdapter::currentChanged, &adapter, [&]() {
        if (adapter.current()->generation().value() == 2U) {
            EXPECT_EQ(adapter.current()->profileId(), QStringLiteral("forge"));
            EXPECT_EQ(adapter.current()->panel()->surfaceColor().red(), 0x34);
            EXPECT_EQ(adapter.current()->notification()->surfaceColor().red(), 0x49);
            reentrant.emplace(adapter.applySnapshot(third.value().snapshot));
        }
    });

    ASSERT_TRUE(adapter.applySnapshot(second.value().snapshot));
    ASSERT_TRUE(reentrant.has_value());
    EXPECT_FALSE(reentrant->hasValue());
    EXPECT_EQ(reentrant->error().code, foundation::ErrorCode::cancelled);
    ASSERT_NE(adapter.current(), nullptr);
    EXPECT_EQ(adapter.current()->generation().value(), 2U);
    EXPECT_EQ(adapter.current()->profileId(), QStringLiteral("forge"));
    ASSERT_TRUE(adapter.previousGeneration());
    EXPECT_EQ(adapter.previousGeneration()->generation().value(), 1U);

    EXPECT_TRUE(adapter.applySnapshot(third.value().snapshot));
    EXPECT_EQ(adapter.current()->generation().value(), 3U);
    EXPECT_EQ(adapter.current()->profileId(), QStringLiteral("lustre"));
}

} // namespace
} // namespace prismdrake::shell::theme
