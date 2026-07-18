#pragma once

#include "DevelopmentNotificationOwner.hpp"
#include "FatalDisplayShutdown.hpp"
#include "LauncherController.hpp"
#include "PanelWindowHost.hpp"
#include "Result.hpp"
#include "SettingsSnapshotClient.hpp"
#include "ShellRuntimeOptions.hpp"
#include "ShellRuntimeState.hpp"
#include "ShellThemeSnapshotAdapter.hpp"
#include "TaskController.hpp"
#include "TaskPresentationModel.hpp"

#include <QObject>
#include <QString>

#include <memory>
#include <span>
#include <vector>

class QQuickView;

namespace prismdrake::shell::runtime {

/// Production composition root for the PD1 Qt/X11 shell process.
///
/// The runtime owns presentation and interaction only. Task state and focus-changing requests
/// remain authoritative in the active EWMH window manager; settings remain authoritative in
/// settingsd. All filesystem discovery, search, process launch, D-Bus calls, and X11 event waits
/// are delegated to the existing asynchronous/event-driven boundaries.
class ShellRuntime final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<ShellRuntime>>
    create(ShellRuntimeOptions options);
    ~ShellRuntime() override;

    ShellRuntime(const ShellRuntime &) = delete;
    ShellRuntime &operator=(const ShellRuntime &) = delete;

    [[nodiscard]] foundation::Result<void> start();

  private slots:
    void handleSettingsSnapshotChanged();
    void handleSettingsStateChanged();
    void openLauncher();
    void dismissLauncherToPanel();
    void dismissLauncherWithoutPanelFocus();
    void handleLauncherSearch(const QString &query);
    void leavePanelKeyboardNavigation();
    void showDevelopmentNotification();
    void
    handleDevelopmentNotificationAction(prismdrake::notifications::NotificationId notificationId,
                                        foundation::Generation contentGeneration,
                                        const QString &actionId);
    void
    handleDevelopmentNotificationDismissal(prismdrake::notifications::NotificationId notificationId,
                                           foundation::Generation contentGeneration);
    void returnNotificationFocusToPanel();

  private:
    explicit ShellRuntime(ShellRuntimeOptions options);

    [[nodiscard]] foundation::Result<void> initialize();
    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);
    [[nodiscard]] foundation::Result<void>
    createPresentationEpoch(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);
    [[nodiscard]] foundation::Result<void>
    updatePresentationEpoch(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);
    [[nodiscard]] foundation::Result<void>
    executeActions(std::span<const RuntimeAction> actions,
                   std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot = {});
    [[nodiscard]] foundation::Result<void> createViews();
    [[nodiscard]] foundation::Result<void> publishThemeProperties();
    [[nodiscard]] foundation::Result<std::uint32_t> projectedPanelHeight() const;
    void positionLauncher();
    void positionNotification();
    void showRetainedNotification();
    void destroyPresentationEpoch() noexcept;
    void handleX11Loss(const foundation::Error &error);
    void reportRecoverable(const char *context, const foundation::Error &error);
    void requestShutdown(const char *context, const foundation::Error &error);

    ShellRuntimeOptions options_;
    ShellRuntimeState state_;
    tasks::TaskPresentationModel task_model_;
    std::unique_ptr<DevelopmentNotificationOwner> notification_owner_;
    std::unique_ptr<launcher::controller::LauncherController> launcher_controller_;
    std::unique_ptr<FatalDisplayShutdown> fatal_display_shutdown_;
    std::unique_ptr<taskcontroller::TaskController> task_controller_;
    settings::SettingsSnapshotClient settings_client_;
    std::unique_ptr<theme::ShellThemeSnapshotAdapter> theme_adapter_;
    std::unique_ptr<QQuickView> panel_view_;
    std::unique_ptr<QQuickView> launcher_view_;
    std::unique_ptr<QQuickView> notification_view_;
    std::unique_ptr<window::PanelWindowHost> panel_host_;
    bool started_{false};
    bool launcher_was_active_{false};
    bool shutdown_requested_{false};
    QString last_recoverable_diagnostic_;
};

} // namespace prismdrake::shell::runtime
