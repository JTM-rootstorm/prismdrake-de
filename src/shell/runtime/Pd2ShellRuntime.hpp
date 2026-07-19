#pragma once

#include "AsyncPinStateController.hpp"
#include "DesktopSurfaceController.hpp"
#include "DesktopWindowHost.hpp"
#include "FatalDisplayShutdown.hpp"
#include "GroupedTaskPresentationModel.hpp"
#include "LauncherController.hpp"
#include "NotificationRuntimeAdapter.hpp"
#include "PanelSurfaceHost.hpp"
#include "QuickSettingsModel.hpp"
#include "SessionControlClient.hpp"
#include "SettingsProfileControlClient.hpp"
#include "SettingsSnapshotClient.hpp"
#include "ShellControlBusOwner.hpp"
#include "ShellOutputMonitor.hpp"
#include "ShellRuntimeComposition.hpp"
#include "ShellRuntimeOptions.hpp"
#include "ShellRuntimeState.hpp"
#include "ShellThemeSnapshotAdapter.hpp"
#include "TaskController.hpp"
#include "TaskPresentationModel.hpp"
#include "WallpaperLoader.hpp"

#include <QObject>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

class QQuickView;
class QScreen;

namespace prismdrake::shell::runtime {

/// Owner-epoch gate for the decoded wallpaper candidate used by runtime surface updates.
/// The desktop host retains its last complete mapped snapshot independently while this gate is
/// empty during a settings-owner gap or replacement decode.
class SettingsWallpaperEpochGate final {
  public:
    void ownerLost() noexcept;

    /// Publishes only a candidate belonging to the active owner and settings generation.
    [[nodiscard]] bool
    publish(std::uint64_t candidateOwnerEpoch, std::uint64_t activeOwnerEpoch,
            std::uint64_t activeSettingsGeneration,
            std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper) noexcept;

    /// Returns a candidate only when both active authority coordinates still match.
    [[nodiscard]] std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot>
    current(std::uint64_t activeOwnerEpoch, std::uint64_t activeSettingsGeneration) const noexcept;

  private:
    struct Candidate final {
        std::uint64_t ownerEpoch;
        std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper;
    };
    std::optional<Candidate> candidate_;
};

/// Production PD2 shell composition root. All authority identities remain in C++; QML receives
/// only presentation models, output names, translated text, and categorical state.
class Pd2ShellRuntime final : public QObject, public ShellRuntimePresentationPort {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<Pd2ShellRuntime>>
    create(ShellRuntimeOptions options);
    ~Pd2ShellRuntime() override;

    [[nodiscard]] foundation::Result<void> start();

    [[nodiscard]] control::ShellRequestFailure
    apply(std::span<const composition::CompositionIntent> intents) override;
    [[nodiscard]] control::ShellRequestFailure
    focusPanel(const composition::OutputName &output) override;
    [[nodiscard]] control::ShellRequestFailure releasePanelKeyboardAccess() override;

  private:
    struct PendingSettingsPublication;
    explicit Pd2ShellRuntime(ShellRuntimeOptions options);
    [[nodiscard]] foundation::Result<void> initialize();
    void handleSettings();
    void loseSettings();
    [[nodiscard]] foundation::Result<void>
    applySettings(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);
    [[nodiscard]] foundation::Result<void>
    applySurfaces(const outputs::SurfaceRegistrySnapshot &snapshot);
    void handlePanelPublication(const outputs::SurfaceRegistrySnapshot &snapshot,
                                const foundation::Result<void> &result);
    [[nodiscard]] foundation::Result<void>
    finishSettingsPublication(const outputs::SurfaceRegistrySnapshot &snapshot);
    [[nodiscard]] foundation::Result<std::unique_ptr<QQuickView>>
    makePanel(std::string_view outputName, QScreen &screen);
    [[nodiscard]] foundation::Result<void> ensureSingletonViews();
    [[nodiscard]] foundation::Result<void> applyWallpaper();
    [[nodiscard]] foundation::Result<void> maybePublishReady();
    [[nodiscard]] foundation::Result<void> position(composition::SurfaceFamily family,
                                                    std::string_view outputName);
    [[nodiscard]] control::ShellRequestFailure
    validatePresentation(std::span<const composition::CompositionIntent> intents) const;
    void handleSessionState(controlclients::SessionStatePublication publication);
    [[nodiscard]] foundation::Result<void> presentActionMenu(std::string_view outputName,
                                                             composition::FocusControlRole role,
                                                             std::string_view controlId);
    void dismissActionMenu();
    [[nodiscard]] foundation::Result<void> reconcileGroupedTasks();
    [[nodiscard]] foundation::Result<void>
    replacePin(const prismdrake::launcher::DesktopFileId &desktopFileId, bool pinned);
    [[nodiscard]] control::ShellRequestFailure
    restoreControlFocus(const composition::FocusOrigin &origin);
    [[nodiscard]] QQuickView *surface(composition::SurfaceFamily family) const noexcept;
    void report(const char *context, const foundation::Error &error);
    void shutdown(const char *context, const foundation::Error &error);
    [[nodiscard]] static QScreen *screen(std::string_view outputName);

  private Q_SLOTS:
    void panelLauncherRequested();
    void panelNotificationCenterRequested(const QString &outputName);
    void panelQuickSettingsRequested(const QString &outputName);
    void panelActionMenuOpened(const QString &outputName);
    void panelActionMenuClosed();
    void launcherSearchRequested(const QString &query);
    void launcherDismissRequested();
    void launcherActionMenuOpened();
    void launcherActionMenuClosed();
    void quickSettingsDismissRequested();
    void notificationCenterDismissRequested();
    void logoutConfirmRequested();
    void logoutCancelRequested();

  private:
    ShellRuntimeOptions options_;
    tasks::TaskPresentationModel task_model_;
    panel::GroupedTaskPresentationModel grouped_tasks_;
    settings::SettingsSnapshotClient settings_client_;
    quicksettings::QuickSettingsModel quick_settings_;
    controlclients::SettingsProfileControlClient profile_control_;
    controlclients::SessionControlClient session_control_;
    ShellRuntimeComposition composition_;
    ShellReadinessGate readiness_gate_;
    ShellControlBusOwner control_bus_;
    NotificationRuntimeAdapter notifications_;
    std::unique_ptr<launcher::controller::LauncherController> launcher_;
    std::unique_ptr<panel::AsyncPinStateController> pin_state_;
    std::unique_ptr<taskcontroller::TaskController> tasks_;
    std::unique_ptr<FatalDisplayShutdown> fatal_display_;
    std::unique_ptr<ShellOutputMonitor> outputs_;
    std::unique_ptr<window::PanelSurfaceHost> panels_;
    desktop::DesktopSurfaceController desktop_controller_;
    std::unique_ptr<desktop::DesktopWindowHost> desktops_;
    prismdrake::desktop::WallpaperLoader wallpaper_loader_;
    std::unique_ptr<theme::ShellThemeSnapshotAdapter> theme_;
    std::unique_ptr<QQuickView> launcher_view_;
    std::unique_ptr<QQuickView> quick_settings_view_;
    std::unique_ptr<QQuickView> notification_center_view_;
    std::unique_ptr<QQuickView> transient_view_;
    std::unique_ptr<QQuickView> logout_view_;
    std::optional<outputs::SurfaceRegistrySnapshot> surface_snapshot_;
    SettingsWallpaperEpochGate wallpaper_candidate_;
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> settings_snapshot_;
    const theme::ShellThemeSnapshotAdapter *panel_theme_candidate_{nullptr};
    std::optional<outputs::SurfaceRegistrySnapshot> pending_surface_snapshot_;
    std::unique_ptr<PendingSettingsPublication> pending_settings_publication_;
    std::uint64_t settings_owner_epoch_{0U};
    std::uint64_t association_generation_{0U};
    std::optional<controlclients::LogoutTransactionId> logout_transaction_;
    std::optional<composition::FocusControlRole> action_menu_role_;
    bool settings_available_{false};
    bool settings_publication_pending_{false};
    bool started_{false};
    bool stopping_{false};
};

} // namespace prismdrake::shell::runtime
