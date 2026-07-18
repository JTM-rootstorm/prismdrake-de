#pragma once

#include "ApplicationCatalog.hpp"
#include "LauncherPresentationModel.hpp"
#include "SettingsEngine.hpp"
#include "ShellThemeSnapshotAdapter.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class QQmlEngine;

namespace prismdrake::shell::launcher::test {

class LauncherSurfaceQmlFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *themeGeneration READ themeGeneration NOTIFY themeGenerationChanged)
    Q_PROPERTY(QAbstractItemModel *launcherModel READ launcherModel NOTIFY launcherModelChanged)
    Q_PROPERTY(int launchCount READ launchCount NOTIFY launchCaptured)
    Q_PROPERTY(QString lastLaunchedName READ lastLaunchedName NOTIFY launchCaptured)
    Q_PROPERTY(QString capturedSearch READ capturedSearch NOTIFY searchCaptured)

  public:
    explicit LauncherSurfaceQmlFixture(QObject *parent = nullptr);
    ~LauncherSurfaceQmlFixture() override;

    [[nodiscard]] QObject *themeGeneration() const noexcept;
    [[nodiscard]] QAbstractItemModel *launcherModel() const noexcept;
    [[nodiscard]] int launchCount() const noexcept { return launch_count_; }
    [[nodiscard]] const QString &lastLaunchedName() const noexcept { return last_launched_name_; }
    [[nodiscard]] const QString &capturedSearch() const noexcept { return captured_search_; }

    Q_INVOKABLE bool resetLustre();
    Q_INVOKABLE bool resetAccessible();
    Q_INVOKABLE bool publishForge();
    Q_INVOKABLE bool publishRepresentativeResults();
    Q_INVOKABLE bool publishLongResult();
    Q_INVOKABLE bool publishLoading();
    Q_INVOKABLE bool publishNoResults();
    Q_INVOKABLE bool publishEmptyCatalog();
    Q_INVOKABLE bool publishError();
    Q_INVOKABLE bool publishReorderedResults();
    Q_INVOKABLE bool removeResult(int row);
    Q_INVOKABLE bool rejectInvalidPublication();
    Q_INVOKABLE void captureSearch(const QString &query);

  signals:
    void themeGenerationChanged();
    void launcherModelChanged();
    void launchCaptured();
    void searchCaptured();

  private:
    struct FixtureApplication final {
        std::string id;
        std::string name;
        std::string genericName;
        std::string comment;
        bool terminal{false};
    };

    [[nodiscard]] bool resetFromConfiguration(const std::filesystem::path &configuration);
    [[nodiscard]] bool rebuildCatalog();
    [[nodiscard]] bool publishSearch(std::string_view query, std::size_t workUnits);
    void captureLaunch(const ApplicationLaunchIntent &intent);
    void removeTemporaryDirectory();

    std::filesystem::path temporary_directory_;
    std::unique_ptr<prismdrake::settings::SettingsEngine> settings_;
    std::unique_ptr<prismdrake::shell::theme::ShellThemeSnapshotAdapter> theme_adapter_;
    std::unique_ptr<LauncherPresentationModel> launcher_presentation_;
    std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog_;
    std::vector<FixtureApplication> applications_;
    std::uint64_t catalog_generation_{6U};
    std::uint64_t request_generation_{0U};
    int launch_count_{0};
    QString last_launched_name_;
    QString captured_search_;
};

class LauncherSurfaceQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::launcher::test
