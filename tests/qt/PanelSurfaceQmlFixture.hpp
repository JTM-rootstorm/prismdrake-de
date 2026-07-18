#pragma once

#include "SettingsEngine.hpp"
#include "ShellThemeSnapshotAdapter.hpp"
#include "TaskModel.hpp"
#include "TaskPresentationModel.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class QQmlEngine;

namespace prismdrake::shell::panel::test {

class PanelSurfaceQmlFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *themeGeneration READ themeGeneration NOTIFY themeGenerationChanged)
    Q_PROPERTY(QAbstractItemModel *taskModel READ taskModel NOTIFY taskModelChanged)
    Q_PROPERTY(int activationCount READ activationCount NOTIFY activationCaptured)
    Q_PROPERTY(QString lastActivationTitle READ lastActivationTitle NOTIFY activationCaptured)
    Q_PROPERTY(
        QString lastActivationGeneration READ lastActivationGeneration NOTIFY activationCaptured)
    Q_PROPERTY(QString taskGeneration READ taskGeneration NOTIFY taskModelChanged)
    Q_PROPERTY(int minimizationCount READ minimizationCount NOTIFY minimizationCaptured)
    Q_PROPERTY(QString lastMinimizationTitle READ lastMinimizationTitle NOTIFY minimizationCaptured)
    Q_PROPERTY(QString lastMinimizationGeneration READ lastMinimizationGeneration NOTIFY
                   minimizationCaptured)
    Q_PROPERTY(int closeCount READ closeCount NOTIFY closeCaptured)
    Q_PROPERTY(QString lastCloseTitle READ lastCloseTitle NOTIFY closeCaptured)
    Q_PROPERTY(QString lastCloseGeneration READ lastCloseGeneration NOTIFY closeCaptured)

  public:
    explicit PanelSurfaceQmlFixture(QObject *parent = nullptr);
    ~PanelSurfaceQmlFixture() override;

    [[nodiscard]] QObject *themeGeneration() const noexcept;
    [[nodiscard]] QAbstractItemModel *taskModel() const noexcept;
    [[nodiscard]] int activationCount() const noexcept { return activation_count_; }
    [[nodiscard]] const QString &lastActivationTitle() const noexcept {
        return last_activation_title_;
    }
    [[nodiscard]] const QString &lastActivationGeneration() const noexcept {
        return last_activation_generation_;
    }
    [[nodiscard]] QString taskGeneration() const;
    [[nodiscard]] int minimizationCount() const noexcept { return minimization_count_; }
    [[nodiscard]] const QString &lastMinimizationTitle() const noexcept {
        return last_minimization_title_;
    }
    [[nodiscard]] const QString &lastMinimizationGeneration() const noexcept {
        return last_minimization_generation_;
    }
    [[nodiscard]] int closeCount() const noexcept { return close_count_; }
    [[nodiscard]] const QString &lastCloseTitle() const noexcept { return last_close_title_; }
    [[nodiscard]] const QString &lastCloseGeneration() const noexcept {
        return last_close_generation_;
    }

    Q_INVOKABLE bool resetLustre();
    Q_INVOKABLE bool resetLustreMissingBlur();
    Q_INVOKABLE bool resetAccessible();
    Q_INVOKABLE bool resetReducedMotion();
    Q_INVOKABLE bool resetTransparencyDisabled();
    Q_INVOKABLE bool publishForge();
    Q_INVOKABLE bool publishRepresentativeTasks();
    Q_INVOKABLE bool swapFirstTwoTasks();
    Q_INVOKABLE bool removeTask(int row);
    Q_INVOKABLE bool setTaskMinimized(int row, bool minimized);

  signals:
    void themeGenerationChanged();
    void taskModelChanged();
    void activationCaptured();
    void minimizationCaptured();
    void closeCaptured();

  private:
    struct FixtureTask final {
        std::uint32_t window;
        std::uint64_t incarnation;
        std::string title;
        std::string applicationId;
        std::string icon;
        bool minimized{false};
        bool urgent{false};
        bool modal{false};
    };

    [[nodiscard]] bool resetFromConfiguration(
        const std::filesystem::path &configuration,
        prismdrake::theme::ThemeResolveOptions capabilities = {{true, true}, false});
    [[nodiscard]] bool resetFromConfigurationText(
        std::string_view configuration,
        prismdrake::theme::ThemeResolveOptions capabilities = {{true, true}, false});
    [[nodiscard]] bool publishTasks();
    void captureActivation(prismdrake::x11::TaskLifetimeId lifetime,
                           prismdrake::x11::TaskModelGeneration generation);
    void captureMinimization(prismdrake::x11::TaskLifetimeId lifetime,
                             prismdrake::x11::TaskModelGeneration generation);
    void captureClose(prismdrake::x11::TaskLifetimeId lifetime,
                      prismdrake::x11::TaskModelGeneration generation);
    void removeTemporaryDirectory();

    std::filesystem::path temporary_directory_;
    std::unique_ptr<prismdrake::settings::SettingsEngine> settings_;
    std::unique_ptr<prismdrake::shell::theme::ShellThemeSnapshotAdapter> theme_adapter_;
    std::unique_ptr<prismdrake::x11::TaskModel> task_source_;
    std::unique_ptr<prismdrake::shell::tasks::TaskPresentationModel> task_presentation_;
    std::vector<FixtureTask> tasks_;
    int activation_count_{0};
    QString last_activation_title_;
    QString last_activation_generation_;
    int minimization_count_{0};
    QString last_minimization_title_;
    QString last_minimization_generation_;
    int close_count_{0};
    QString last_close_title_;
    QString last_close_generation_;
};

class PanelSurfaceQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::panel::test
