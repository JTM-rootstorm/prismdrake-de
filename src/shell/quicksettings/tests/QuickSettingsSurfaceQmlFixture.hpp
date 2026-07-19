#pragma once

#include "SettingsEngine.hpp"
#include "ShellThemeSnapshotAdapter.hpp"

#include <QObject>

#include <filesystem>
#include <memory>
#include <string_view>

class QQmlEngine;

namespace prismdrake::shell::quicksettings::test {

class QuickSettingsSurfaceQmlFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *themeGeneration READ themeGeneration NOTIFY themeGenerationChanged)

  public:
    explicit QuickSettingsSurfaceQmlFixture(QObject *parent = nullptr);
    ~QuickSettingsSurfaceQmlFixture() override;

    [[nodiscard]] QObject *themeGeneration() const noexcept;

    Q_INVOKABLE bool resetLustre();
    Q_INVOKABLE bool resetLustreMissingBlur();
    Q_INVOKABLE bool resetAccessible();
    Q_INVOKABLE bool rejectInvalidCandidate();

  signals:
    void themeGenerationChanged();

  private:
    [[nodiscard]] bool resetFromConfiguration(
        const std::filesystem::path &configuration,
        prismdrake::theme::ThemeResolveOptions capabilities = {{true, true}, false});
    [[nodiscard]] bool resetFromConfigurationText(
        std::string_view configuration,
        prismdrake::theme::ThemeResolveOptions capabilities = {{true, true}, false});
    void removeTemporaryDirectory();

    std::filesystem::path temporary_directory_;
    std::unique_ptr<prismdrake::settings::SettingsEngine> settings_;
    std::unique_ptr<prismdrake::shell::theme::ShellThemeSnapshotAdapter> theme_adapter_;
};

class QuickSettingsSurfaceQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::quicksettings::test
