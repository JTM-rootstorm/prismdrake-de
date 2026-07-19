#include "QuickSettingsSurfaceQmlFixture.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <atomic>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace prismdrake::shell::quicksettings::test {
namespace {

[[nodiscard]] bool writeTextFile(const std::filesystem::path &destination,
                                 std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output{destination, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

[[nodiscard]] std::optional<std::string> readTextFile(const std::filesystem::path &source) {
    std::ifstream input{source, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return contents.str();
}

} // namespace

QuickSettingsSurfaceQmlFixture::QuickSettingsSurfaceQmlFixture(QObject *parent) : QObject(parent) {
    static_cast<void>(resetLustre());
}

QuickSettingsSurfaceQmlFixture::~QuickSettingsSurfaceQmlFixture() {
    theme_adapter_.reset();
    settings_.reset();
    removeTemporaryDirectory();
}

QObject *QuickSettingsSurfaceQmlFixture::themeGeneration() const noexcept {
    return theme_adapter_ ? theme_adapter_->current() : nullptr;
}

bool QuickSettingsSurfaceQmlFixture::resetLustre() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/lustre.toml");
}

bool QuickSettingsSurfaceQmlFixture::resetLustreMissingBlur() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                      "examples/config/lustre.toml",
                                  {{false, true}, false});
}

bool QuickSettingsSurfaceQmlFixture::resetAccessible() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/accessible.toml");
}

bool QuickSettingsSurfaceQmlFixture::rejectInvalidCandidate() {
    if (!settings_ || !theme_adapter_ || !theme_adapter_->current()) {
        return false;
    }
    const auto *retained = theme_adapter_->current();
    const auto validation =
        settings_->validateCandidate("schema_version = 1\nprivate_secret = \"must-not-publish\"\n");
    return validation && !validation.value().valid && theme_adapter_->current() == retained;
}

bool QuickSettingsSurfaceQmlFixture::resetFromConfiguration(
    const std::filesystem::path &configuration,
    prismdrake::theme::ThemeResolveOptions capabilities) {
    const auto document = readTextFile(configuration);
    return document && resetFromConfigurationText(*document, capabilities);
}

bool QuickSettingsSurfaceQmlFixture::resetFromConfigurationText(
    std::string_view configuration, prismdrake::theme::ThemeResolveOptions capabilities) {
    static std::atomic_uint sequence{0U};
    const auto nextTemporaryDirectory =
        std::filesystem::temp_directory_path() /
        ("prismdrake-quick-settings-qml-test-" + std::to_string(sequence.fetch_add(1U)));
    const auto userConfiguration = nextTemporaryDirectory / "config/config.toml";
    if (!writeTextFile(userConfiguration, configuration)) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{
        userConfiguration, nextTemporaryDirectory / "state/last-known-valid-config.toml",
        source / "data/defaults/config.toml"};
    auto started = prismdrake::settings::SettingsEngine::start(
        {locations, source / "themes", {}, capabilities});
    if (!started) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    auto nextSettings = std::move(started).value();
    auto nextTheme = std::make_unique<prismdrake::shell::theme::ShellThemeSnapshotAdapter>();
    if (!nextTheme->applySnapshot(nextSettings->current())) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }
    connect(nextTheme.get(), &prismdrake::shell::theme::ShellThemeSnapshotAdapter::currentChanged,
            this, &QuickSettingsSurfaceQmlFixture::themeGenerationChanged);

    const auto previousTemporaryDirectory = std::move(temporary_directory_);
    auto previousSettings = std::move(settings_);
    auto previousTheme = std::move(theme_adapter_);

    temporary_directory_ = nextTemporaryDirectory;
    settings_ = std::move(nextSettings);
    theme_adapter_ = std::move(nextTheme);
    emit themeGenerationChanged();

    previousTheme.reset();
    previousSettings.reset();
    if (!previousTemporaryDirectory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(previousTemporaryDirectory, error);
    }
    return true;
}

void QuickSettingsSurfaceQmlFixture::removeTemporaryDirectory() {
    if (temporary_directory_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(temporary_directory_, error);
    temporary_directory_.clear();
}

void QuickSettingsSurfaceQmlSetup::qmlEngineAvailable(QQmlEngine *engine) {
    engine->rootContext()->setContextProperty(QStringLiteral("quickSettingsThemeFixture"),
                                              new QuickSettingsSurfaceQmlFixture(engine));
}

} // namespace prismdrake::shell::quicksettings::test
