#pragma once

#include "SettingsEngine.hpp"
#include "ShellThemeSnapshotAdapter.hpp"

#include <QObject>

#include <filesystem>
#include <memory>

namespace prismdrake::shell::visual::test {

/// Provides production settings/theme generations to surface-independent visual tests.
class VisualThemeFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *themeGeneration READ themeGeneration NOTIFY themeGenerationChanged)

  public:
    explicit VisualThemeFixture(QObject *parent = nullptr);
    ~VisualThemeFixture() override;

    [[nodiscard]] QObject *themeGeneration() const noexcept;

    Q_INVOKABLE bool resetLustre();
    Q_INVOKABLE bool resetAccessible();
    Q_INVOKABLE bool publishForge();

  signals:
    void themeGenerationChanged();

  private:
    [[nodiscard]] bool resetFromConfiguration(const std::filesystem::path &configuration);
    void removeTemporaryDirectory();

    std::filesystem::path temporary_directory_;
    std::unique_ptr<prismdrake::settings::SettingsEngine> settings_;
    std::unique_ptr<prismdrake::shell::theme::ShellThemeSnapshotAdapter> theme_adapter_;
};

} // namespace prismdrake::shell::visual::test
