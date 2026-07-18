#include "VisualThemeFixture.hpp"

#include <atomic>
#include <optional>
#include <system_error>
#include <utility>

namespace prismdrake::shell::visual::test {
namespace {

[[nodiscard]] bool copyTextFile(const std::filesystem::path &source,
                                const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return false;
    }
    return std::filesystem::copy_file(source, destination,
                                      std::filesystem::copy_options::overwrite_existing, error) &&
           !error;
}

} // namespace

VisualThemeFixture::VisualThemeFixture(QObject *parent) : QObject(parent) {
    static_cast<void>(resetLustre());
}

VisualThemeFixture::~VisualThemeFixture() {
    theme_adapter_.reset();
    settings_.reset();
    removeTemporaryDirectory();
}

QObject *VisualThemeFixture::themeGeneration() const noexcept {
    return theme_adapter_ ? theme_adapter_->current() : nullptr;
}

bool VisualThemeFixture::resetLustre() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/lustre.toml");
}

bool VisualThemeFixture::resetAccessible() {
    return resetFromConfiguration(std::filesystem::path(PRISMDRAKE_SOURCE_DIR) /
                                  "examples/config/accessible.toml");
}

bool VisualThemeFixture::publishForge() {
    if (!settings_ || !theme_adapter_) {
        return false;
    }
    auto publication = settings_->requestProfileChange("forge");
    return publication && theme_adapter_->applySnapshot(publication.value().snapshot).hasValue();
}

bool VisualThemeFixture::resetFromConfiguration(const std::filesystem::path &configuration) {
    static std::atomic_uint sequence{0U};
    const auto nextTemporaryDirectory =
        std::filesystem::temp_directory_path() /
        ("prismdrake-visual-theme-test-" + std::to_string(sequence.fetch_add(1U)));
    const auto userConfiguration = nextTemporaryDirectory / "config/config.toml";
    if (!copyTextFile(configuration, userConfiguration)) {
        std::error_code error;
        std::filesystem::remove_all(nextTemporaryDirectory, error);
        return false;
    }

    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{
        userConfiguration, nextTemporaryDirectory / "state/last-known-valid-config.toml",
        source / "data/defaults/config.toml"};
    constexpr prismdrake::theme::ThemeResolveOptions capabilities{{true, true}, false};
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
            this, &VisualThemeFixture::themeGenerationChanged);

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

void VisualThemeFixture::removeTemporaryDirectory() {
    if (temporary_directory_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(temporary_directory_, error);
    temporary_directory_.clear();
}

} // namespace prismdrake::shell::visual::test
