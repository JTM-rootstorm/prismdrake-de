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

    Q_INVOKABLE bool resetLustre();
    Q_INVOKABLE bool resetAccessible();
    Q_INVOKABLE bool publishForge();
    Q_INVOKABLE bool publishRepresentativeTasks();
    Q_INVOKABLE bool swapFirstTwoTasks();
    Q_INVOKABLE bool removeTask(int row);

  signals:
    void themeGenerationChanged();
    void taskModelChanged();
    void activationCaptured();

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

    [[nodiscard]] bool resetFromConfiguration(const std::filesystem::path &configuration);
    [[nodiscard]] bool publishTasks();
    void captureActivation(prismdrake::x11::TaskLifetimeId lifetime,
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
};

class PanelSurfaceQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::panel::test
