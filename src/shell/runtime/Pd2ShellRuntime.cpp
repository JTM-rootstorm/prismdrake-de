#include "Pd2ShellRuntime.hpp"

#include "ExitStatus.hpp"
#include "WallpaperModel.hpp"
#include "XdgPaths.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickView>
#include <QScreen>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <QDebug>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error surfaceError() {
    return {ErrorCode::invalid_environment, "A production shell QML surface failed to load.",
            "Install matching Prismdrake QML modules and restart the exact shell child."};
}

[[nodiscard]] QVariant objectVariant(QObject *object) { return QVariant::fromValue(object); }

[[nodiscard]] outputs::PanelPlacementPolicy panelPolicy(const config::Panel &panel) {
    outputs::PanelOutputPolicy policy = outputs::PanelOutputPolicy::primary;
    if (panel.outputPolicy == config::PanelOutputPolicy::all) {
        policy = outputs::PanelOutputPolicy::all;
    } else if (panel.outputPolicy == config::PanelOutputPolicy::named) {
        policy = outputs::PanelOutputPolicy::named;
    }
    return {policy, panel.outputs};
}

[[nodiscard]] Result<x11::WindowId> windowId(QWindow &window) {
    const auto native = window.winId();
    if (native == 0U || native > std::numeric_limits<x11::WindowId::Value>::max()) {
        return Result<x11::WindowId>::failure(surfaceError());
    }
    return x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(native));
}

} // namespace

void SettingsWallpaperEpochGate::ownerLost() noexcept { candidate_.reset(); }

bool SettingsWallpaperEpochGate::publish(
    std::uint64_t candidateOwnerEpoch, std::uint64_t activeOwnerEpoch,
    std::uint64_t activeSettingsGeneration,
    std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper) noexcept {
    if (candidateOwnerEpoch == 0U || candidateOwnerEpoch != activeOwnerEpoch ||
        activeSettingsGeneration == 0U || !wallpaper ||
        wallpaper->generation != activeSettingsGeneration) {
        return false;
    }
    candidate_ = Candidate{candidateOwnerEpoch, std::move(wallpaper)};
    return true;
}

std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot>
SettingsWallpaperEpochGate::current(std::uint64_t activeOwnerEpoch,
                                    std::uint64_t activeSettingsGeneration) const noexcept {
    if (!candidate_ || candidate_->ownerEpoch != activeOwnerEpoch || !candidate_->wallpaper ||
        candidate_->wallpaper->generation != activeSettingsGeneration) {
        return {};
    }
    return candidate_->wallpaper;
}

struct Pd2ShellRuntime::PendingSettingsPublication final {
    std::unique_ptr<theme::ShellThemeSnapshotAdapter> theme;
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> settings;
    prismdrake::desktop::WallpaperRequest wallpaperRequest;
};

struct Pd2ShellRuntime::PendingSingletonPresentation final {
    composition::SurfaceFamily family;
    composition::OutputName output;
    x11::WindowId window;
    bool focus;
};

Pd2ShellRuntime::Pd2ShellRuntime(ShellRuntimeOptions options)
    : options_(std::move(options)), task_model_(this), grouped_tasks_(this), settings_client_(this),
      quick_settings_(this),
      profile_control_(
          [this](auto requestId, auto completion) {
              auto applied = quick_settings_.completeProfileRequest(requestId, completion);
              if (!applied) {
                  report("profile completion", applied.error());
              }
          },
          this),
      session_control_(
          [this](const auto &completion) {
              if (completion.failure == quicksettings::CommandFailure::none ||
                  (completion.transaction &&
                   (!logout_transaction_ || *completion.transaction != *logout_transaction_))) {
                  return;
              }
              if (completion.operation == controlclients::SessionControlOperation::request) {
                  if (logout_transaction_) {
                      return;
                  }
                  quick_settings_.observeSessionState(
                      quicksettings::SessionPresentationState::failed);
                  return;
              }
              if (logout_view_ && logout_view_->rootObject()) {
                  logout_view_->rootObject()->setProperty("transactionPending", false);
                  logout_view_->rootObject()->setProperty(
                      "statusText",
                      completion.operation == controlclients::SessionControlOperation::confirm
                          ? tr("Logout could not be confirmed. You may retry or cancel.")
                          : tr("Logout could not be cancelled. You may retry."));
              }
          },
          [this](auto publication) { handleSessionState(std::move(publication)); }, this),
      composition_(*this),
      control_bus_(
          composition_, [this](const auto &error) { shutdown("Shell1", error); }, this),
      notifications_(
          quick_settings_,
          [this](auto authority) {
              auto applied = composition_.observeNotificationOwner(std::move(authority));
              if (!applied) {
                  report("notification authority", applied.error());
              }
          },
          [this](const auto &error) { report("notifications", error); }, this) {
    quick_settings_.setNotificationCenterPort(&composition_);
    quick_settings_.setLogoutControlPort(&session_control_);
}

Pd2ShellRuntime::~Pd2ShellRuntime() {
    stopping_ = true;
    if (pin_state_) {
        pin_state_->stop();
    }
    control_bus_.stop();
    notifications_.stop();
    session_control_.stop();
    profile_control_.stop();
    settings_client_.stop();
    if (outputs_) {
        outputs_->stop();
    }
    singleton_property_worker_.reset();
}

Result<std::unique_ptr<Pd2ShellRuntime>> Pd2ShellRuntime::create(ShellRuntimeOptions options) {
    auto runtime = std::unique_ptr<Pd2ShellRuntime>(new Pd2ShellRuntime(std::move(options)));
    auto initialized = runtime->initialize();
    if (!initialized) {
        return Result<std::unique_ptr<Pd2ShellRuntime>>::failure(initialized.error());
    }
    return Result<std::unique_ptr<Pd2ShellRuntime>>::success(std::move(runtime));
}

Result<void> Pd2ShellRuntime::initialize() {
    auto launcher = launcher::controller::LauncherController::create(std::move(options_.launcher));
    if (!launcher) {
        return Result<void>::failure(launcher.error());
    }
    launcher_ = std::move(launcher).value();
    auto paths = foundation::resolveCurrentProcessXdgPaths();
    if (!paths) {
        return Result<void>::failure(paths.error());
    }
    auto pins = panel::AsyncPinStateController::create(paths.value().stateDirectory, this);
    if (!pins) {
        return Result<void>::failure(pins.error());
    }
    pin_state_ = std::move(pins).value();
    connect(pin_state_.get(), &panel::AsyncPinStateController::snapshotChanged, this, [this]() {
        const auto &snapshot = pin_state_->current();
        if (!snapshot) {
            shutdown("pin state", surfaceError());
            return;
        }
        auto launcherPins = launcher_->applyPinSnapshot(snapshot->generation, snapshot->pins);
        if (!launcherPins) {
            shutdown("pin state publication", launcherPins.error());
            return;
        }
        auto grouped = reconcileGroupedTasks();
        if (!grouped) {
            shutdown("pin state grouping", grouped.error());
            return;
        }
        auto ready = maybePublishReady();
        if (!ready) {
            shutdown("pin state readiness", ready.error());
        }
    });
    connect(pin_state_.get(), &panel::AsyncPinStateController::persistenceFailed, this, [this]() {
        if (!pin_state_->lastError()) {
            shutdown("pin state", surfaceError());
            return;
        }
        if (!pin_state_->current()) {
            shutdown("initial pin state", *pin_state_->lastError());
            return;
        }
        report("pin state", *pin_state_->lastError());
    });
    auto fatal = FatalDisplayShutdown::create(
        [this](const auto &error) { shutdown("X11 connection", error); });
    if (!fatal) {
        return Result<void>::failure(fatal.error());
    }
    fatal_display_ = std::move(fatal).value();
    auto singletonPropertyWorker = window::X11PropertyWorker::create(
        *this, options_.display, [this](std::uint64_t token, Result<void> result) {
            handleSingletonTaskPolicyPublication(token, std::move(result));
        });
    if (!singletonPropertyWorker) {
        return Result<void>::failure(singletonPropertyWorker.error());
    }
    singleton_property_worker_ = std::move(singletonPropertyWorker).value();
    auto taskController = taskcontroller::TaskController::create(
        task_model_, options_.display,
        [this](const auto &error) { fatal_display_->request(error); },
        [this](const auto &error) { report("task refresh", error); },
        [this](const auto &update) {
            if (update.outcome == x11::TaskRequestOutcome::deliveryRejected ||
                update.outcome == x11::TaskRequestOutcome::refused ||
                update.outcome == x11::TaskRequestOutcome::targetReplaced) {
                report("task request",
                       {ErrorCode::cancelled, "The window manager rejected a current task request.",
                        "Retry against the next authoritative task snapshot."});
            }
        });
    if (!taskController) {
        return Result<void>::failure(taskController.error());
    }
    tasks_ = std::move(taskController).value();

    connect(&task_model_, &tasks::TaskPresentationModel::publicationApplied, this, [this]() {
        auto reconciled = reconcileGroupedTasks();
        if (!reconciled) {
            report("grouped tasks", reconciled.error());
        }
    });
    connect(launcher_.get(), &launcher::controller::LauncherController::catalogChanged, this,
            [this]() {
                auto reconciled = reconcileGroupedTasks();
                if (!reconciled) {
                    report("grouped tasks", reconciled.error());
                }
            });
    connect(launcher_.get(), &launcher::controller::LauncherController::pinChangeRequested, this,
            [this](const launcher::ApplicationPinIntent &intent) {
                auto replaced = replacePin(intent.desktopFileId, intent.pin);
                if (!replaced) {
                    report("pin state", replaced.error());
                }
            });
    connect(&grouped_tasks_, &panel::GroupedTaskPresentationModel::pinChangeRequested, this,
            [this](const prismdrake::launcher::DesktopFileId &id, bool pinned) {
                auto replaced = replacePin(id, pinned);
                if (!replaced) {
                    report("pin state", replaced.error());
                }
            });
    connect(&grouped_tasks_, &panel::GroupedTaskPresentationModel::launchRequested, this,
            [this](const prismdrake::launcher::DesktopFileId &id) {
                auto launched = launcher_->launchApplication(id);
                if (!launched) {
                    report("group launch", launched.error());
                }
            });
    const auto requestTask = [this](x11::TaskLifetimeId lifetime,
                                    x11::TaskModelGeneration generation,
                                    x11::TaskRequestAction action) {
        auto requested = tasks_->request(lifetime, generation, action);
        if (!requested) {
            report("grouped task request", requested.error());
        }
    };
    connect(&grouped_tasks_, &panel::GroupedTaskPresentationModel::activationRequested, this,
            [requestTask](auto lifetime, auto generation) {
                requestTask(lifetime, generation, x11::TaskRequestAction::activate);
            });
    connect(&grouped_tasks_, &panel::GroupedTaskPresentationModel::minimizationRequested, this,
            [requestTask](auto lifetime, auto generation) {
                requestTask(lifetime, generation, x11::TaskRequestAction::minimize);
            });
    connect(&grouped_tasks_, &panel::GroupedTaskPresentationModel::closeRequested, this,
            [requestTask](auto lifetime, auto generation) {
                requestTask(lifetime, generation, x11::TaskRequestAction::close);
            });

    auto panelHost = window::PanelSurfaceHost::create(
        options_.display, &Pd2ShellRuntime::screen,
        [this](std::string_view output, QScreen &screenValue) {
            return makePanel(output, screenValue);
        },
        [this](const outputs::SurfaceRegistrySnapshot &snapshot, const Result<void> &result) {
            handlePanelPublication(snapshot, result);
        });
    if (!panelHost) {
        return Result<void>::failure(panelHost.error());
    }
    panels_ = std::move(panelHost).value();
    auto desktopHost = desktop::DesktopWindowHost::create(
        options_.display, &Pd2ShellRuntime::screen,
        [this](const desktop::DesktopSurfaceUpdate &update, const Result<void> &result) {
            if (!result) {
                shutdown("desktop X11 publication", result.error());
                return;
            }
            desktop_controller_.commit(update);
            auto ready = maybePublishReady();
            if (!ready) {
                shutdown("desktop readiness", ready.error());
            }
        });
    if (!desktopHost) {
        return Result<void>::failure(desktopHost.error());
    }
    desktops_ = std::move(desktopHost).value();
    outputs_ = std::make_unique<ShellOutputMonitor>(
        options_.display, [this](const auto &snapshot) { return applySurfaces(snapshot); },
        [this](const auto &error, bool fatalError) {
            if (fatalError) {
                fatal_display_->request(error);
            } else {
                report("output topology", error);
            }
        },
        this);

    connect(&settings_client_, &settings::SettingsSnapshotClient::snapshotChanged, this,
            &Pd2ShellRuntime::handleSettings);
    connect(&settings_client_, &settings::SettingsSnapshotClient::stateChanged, this, [this]() {
        if (settings_client_.state() == settings::SettingsSnapshotClient::State::failed &&
            settings_client_.lastError()) {
            report("settings", *settings_client_.lastError());
        }
    });
    connect(&profile_control_, &controlclients::SettingsProfileControlClient::stateChanged, this,
            [this]() {
                quick_settings_.setProfileControlPort(
                    profile_control_.state() ==
                            controlclients::SettingsProfileControlClient::State::available
                        ? &profile_control_
                        : nullptr);
            });
    connect(&session_control_, &controlclients::SessionControlClient::stateChanged, this, [this]() {
        if (session_control_.state() != controlclients::SessionControlClient::State::available) {
            quick_settings_.setSessionAvailable(false);
        }
    });
    connect(launcher_.get(), &launcher::controller::LauncherController::launchCompleted, this,
            [this]() {
                static_cast<void>(composition_.dismiss(composition::SurfaceFamily::launcher));
            });
    connect(&notifications_.presentation(),
            &notificationcenter::NotificationCenterPresentation::stateChanged, this,
            [this]() { reconcileTransientPresentation(); });
    return Result<void>::success();
}

Result<void> Pd2ShellRuntime::start() {
    if (started_) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The PD2 shell is already started.",
                                      "Start one exact shell owner epoch."});
    }
    auto pins = pin_state_->start();
    if (!pins) {
        return pins;
    }
    auto settings = settings_client_.start();
    if (!settings) {
        return settings;
    }
    auto notifications = notifications_.start();
    if (!notifications) {
        return notifications;
    }
    auto outputMonitor = outputs_->start();
    if (!outputMonitor) {
        return outputMonitor;
    }
    auto profile = profile_control_.start();
    if (!profile) {
        report("profile control", profile.error());
    }
    auto session = session_control_.start();
    if (!session) {
        report("session control", session.error());
    }
    auto control = control_bus_.start();
    if (!control) {
        return control;
    }
    auto refreshed = launcher_->refresh();
    if (!refreshed) {
        return refreshed;
    }
    started_ = true;
    return maybePublishReady();
}

void Pd2ShellRuntime::handleSettings() {
    auto snapshot = settings_client_.currentSnapshot();
    if (!snapshot) {
        loseSettings();
        return;
    }
    auto applied = applySettings(std::move(snapshot));
    if (!applied) {
        shutdown(settings_available_ ? "settings publication" : "initial settings",
                 applied.error());
    }
}

void Pd2ShellRuntime::loseSettings() {
    if (!settings_available_ && !settings_publication_pending_) {
        return;
    }
    settings_available_ = false;
    settings_publication_pending_ = false;
    panel_theme_candidate_ = nullptr;
    pending_settings_publication_.reset();
    pending_surface_snapshot_.reset();
    desktop_controller_.settingsOwnerLost();
    wallpaper_candidate_.ownerLost();
    wallpaper_loader_.beginSettingsOwnerEpoch();
    auto authority = composition_.observeSettingsOwner(std::nullopt);
    outputs_->beginSettingsOwnerEpoch();
    panels_->clear();
    grouped_tasks_.clearSettingsOwner();
    quick_settings_.clearSettingsOwner();
    pending_singleton_presentations_.clear();
    singleton_task_policy_gate_.invalidate();
    if (singleton_task_policy_gate_.pending()) {
        for (auto *view :
             {launcher_view_.get(), quick_settings_view_.get(), notification_center_view_.get(),
              transient_view_.get(), logout_view_.get()}) {
            if (view != nullptr) {
                view->hide();
            }
        }
    } else {
        launcher_view_.reset();
        quick_settings_view_.reset();
        notification_center_view_.reset();
        transient_view_.reset();
        logout_view_.reset();
        theme_.reset();
    }
    settings_snapshot_.reset();
    if (!authority) {
        shutdown("settings owner loss", authority.error());
    }
}

Result<void> Pd2ShellRuntime::applySettings(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    auto candidateTheme = std::make_unique<theme::ShellThemeSnapshotAdapter>();
    auto projected = candidateTheme->applySnapshot(snapshot);
    if (!projected) {
        return projected;
    }
    const auto generation = snapshot->generation.value();
    auto paths = foundation::resolveCurrentProcessXdgPaths();
    if (!paths) {
        return Result<void>::failure(paths.error());
    }
    auto wallpaperRequest = prismdrake::desktop::makeWallpaperRequest(
        snapshot->candidate.configuration.desktop,
        QString::fromStdString(paths.value().configDirectory.string()), generation);
    if (!wallpaperRequest) {
        return Result<void>::failure(wallpaperRequest.error());
    }
    if (!settings_available_ && !settings_publication_pending_) {
        if (settings_owner_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::failure(surfaceError());
        }
        ++settings_owner_epoch_;
    }

    pending_settings_publication_ =
        std::make_unique<PendingSettingsPublication>(PendingSettingsPublication{
            std::move(candidateTheme), snapshot, std::move(wallpaperRequest).value()});
    settings_publication_pending_ = true;
    panel_theme_candidate_ = pending_settings_publication_->theme.get();
    pending_surface_snapshot_.reset();
    auto configured =
        outputs_->configure(snapshot->candidate.configuration.panel.sizePx,
                            panelPolicy(snapshot->candidate.configuration.panel), generation);
    if (!configured) {
        settings_publication_pending_ = false;
        panel_theme_candidate_ = nullptr;
        pending_settings_publication_.reset();
        pending_surface_snapshot_.reset();
        return configured;
    }
    return Result<void>::success();
}

Result<void> Pd2ShellRuntime::finishSettingsPublication(
    const outputs::SurfaceRegistrySnapshot &surfaceSnapshot) {
    if (!pending_settings_publication_ ||
        surfaceSnapshot.settingsGeneration() !=
            pending_settings_publication_->settings->generation.value()) {
        return Result<void>::failure(surfaceError());
    }
    auto pending = std::move(pending_settings_publication_);
    settings_publication_pending_ = false;
    panel_theme_candidate_ = nullptr;
    pending_surface_snapshot_.reset();
    const auto generation = pending->settings->generation.value();
    auto authority = composition_.observeSettingsOwner(composition::AuthoritySnapshot{
        composition::OwnerEpoch::create(settings_owner_epoch_).value(),
        composition::OwnerGeneration::create(generation).value()});
    if (!authority) {
        return authority;
    }
    auto quick = quick_settings_.applySettingsSnapshot(pending->settings);
    if (!quick) {
        return quick;
    }
    // Persistent QML views still reference the prior QObject graph until their
    // themeGeneration properties are replaced below. Keep that graph alive
    // across the complete synchronous property handoff.
    auto previousTheme = std::move(theme_);
    theme_ = std::move(pending->theme);
    settings_snapshot_ = pending->settings;
    settings_available_ = true;
    auto views = ensureSingletonViews();
    if (!views) {
        return views;
    }
    auto outputComposition = composition_.observeOutputs(surfaceSnapshot);
    if (!outputComposition) {
        return outputComposition;
    }
    surface_snapshot_ = surfaceSnapshot;
    const auto themeValue = objectVariant(theme_->current());
    if (surface_snapshot_) {
        for (const auto &placement : surface_snapshot_->panels()) {
            if (auto *panel = panels_->view(placement.outputName); panel && panel->rootObject()) {
                panel->rootObject()->setProperty("themeGeneration", themeValue);
            }
        }
    }
    previousTheme.reset();
    auto grouped = reconcileGroupedTasks();
    if (!grouped) {
        return grouped;
    }
    const auto wallpaperOwnerEpoch = settings_owner_epoch_;
    return wallpaper_loader_.load(
        std::move(pending->wallpaperRequest), this, [this, wallpaperOwnerEpoch](auto wallpaper) {
            // A later settings publication may complete before this asynchronous decode. Never
            // project an older wallpaper or prior-owner candidate into the current desktop.
            if (!settings_snapshot_ ||
                !wallpaper_candidate_.publish(wallpaperOwnerEpoch, settings_owner_epoch_,
                                              settings_snapshot_->generation.value(),
                                              std::move(wallpaper))) {
                return;
            }
            auto applied = applyWallpaper();
            if (!applied) {
                shutdown("desktop wallpaper", applied.error());
            }
        });
}

Result<void> Pd2ShellRuntime::applySurfaces(const outputs::SurfaceRegistrySnapshot &snapshot) {
    if (settings_publication_pending_) {
        auto panels = panels_->apply(snapshot);
        if (!panels) {
            return panels;
        }
        pending_surface_snapshot_ = snapshot;
        return Result<void>::success();
    }
    if (!settings_available_ || !theme_) {
        surface_snapshot_ = snapshot;
        return Result<void>::success();
    }
    return panels_->apply(snapshot);
}

void Pd2ShellRuntime::handlePanelPublication(const outputs::SurfaceRegistrySnapshot &snapshot,
                                             const Result<void> &result) {
    if (!result) {
        settings_publication_pending_ = false;
        panel_theme_candidate_ = nullptr;
        pending_settings_publication_.reset();
        pending_surface_snapshot_.reset();
        shutdown("panel X11 publication", result.error());
        return;
    }
    if (settings_publication_pending_) {
        if (!pending_surface_snapshot_ ||
            pending_surface_snapshot_->surfaceGeneration() != snapshot.surfaceGeneration() ||
            pending_surface_snapshot_->settingsGeneration() != snapshot.settingsGeneration()) {
            settings_publication_pending_ = false;
            panel_theme_candidate_ = nullptr;
            pending_settings_publication_.reset();
            pending_surface_snapshot_.reset();
            shutdown("panel publication generation", surfaceError());
            return;
        }
        auto finished = finishSettingsPublication(snapshot);
        if (!finished) {
            shutdown("settings publication", finished.error());
        }
        return;
    }
    if (!settings_available_ || !theme_) {
        return;
    }
    auto composition = composition_.observeOutputs(snapshot);
    if (!composition) {
        shutdown("output composition", composition.error());
        return;
    }
    surface_snapshot_ = snapshot;
    auto grouped = reconcileGroupedTasks();
    if (!grouped) {
        shutdown("grouped task publication", grouped.error());
        return;
    }
    auto wallpaper = applyWallpaper();
    if (!wallpaper) {
        shutdown("desktop wallpaper", wallpaper.error());
    }
}

Result<void> Pd2ShellRuntime::applyWallpaper() {
    if (!surface_snapshot_ || !settings_snapshot_ || !desktops_) {
        return Result<void>::success();
    }
    const auto wallpaper =
        wallpaper_candidate_.current(settings_owner_epoch_, settings_snapshot_->generation.value());
    if (!wallpaper) {
        return Result<void>::success();
    }
    std::vector<desktop::OutputScaleObservation> scales;
    scales.reserve(surface_snapshot_->desktops().size());
    for (const auto &placement : surface_snapshot_->desktops()) {
        auto *outputScreen = screen(placement.outputName);
        if (outputScreen == nullptr) {
            return Result<void>::failure(surfaceError());
        }
        scales.push_back({placement.outputName, outputScreen->devicePixelRatio()});
    }
    auto update = desktop_controller_.prepare(*surface_snapshot_, scales, wallpaper);
    if (!update) {
        return Result<void>::failure(update.error());
    }
    auto applied = desktops_->apply(update.value());
    if (!applied) {
        return applied;
    }
    return maybePublishReady();
}

Result<void> Pd2ShellRuntime::maybePublishReady() {
    if (!pin_state_ || !pin_state_->current()) {
        return Result<void>::success();
    }
    const auto desktop = desktops_ ? desktops_->current() : nullptr;
    const bool presentationReady = theme_ && theme_->current() && launcher_view_ &&
                                   quick_settings_view_ && notification_center_view_ &&
                                   transient_view_ && logout_view_ &&
                                   singleton_task_policy_gate_.ready();
    const auto settingsGeneration =
        settings_snapshot_ ? settings_snapshot_->generation.value() : 0U;
    const ShellReadinessObservation observation{
        .processStarted = started_,
        .settingsOwnerAvailable = settings_available_,
        .presentationSurfacesReady = presentationReady,
        .hasPrimaryOutput = surface_snapshot_ && surface_snapshot_->primaryOutputName().has_value(),
        .settingsGeneration = settingsGeneration,
        .outputSettingsGeneration =
            surface_snapshot_ ? surface_snapshot_->settingsGeneration() : 0U,
        .outputSurfaceGeneration = surface_snapshot_ ? surface_snapshot_->surfaceGeneration() : 0U,
        .panelSurfaceGeneration = panels_ ? panels_->currentGeneration() : 0U,
        .outputTopologyGeneration =
            surface_snapshot_ ? surface_snapshot_->topologyGeneration() : 0U,
        .desktopTopologyGeneration = desktop ? desktop->topologyGeneration() : 0U,
        .wallpaperSettingsGeneration = desktop ? desktop->wallpaper()->generation : 0U,
        .outputDesktopCount = surface_snapshot_ ? surface_snapshot_->desktops().size() : 0U,
        .desktopSurfaceCount = desktop ? desktop->surfaces().size() : 0U};
    if (!readiness_gate_.consumeIfComplete(observation)) {
        return Result<void>::success();
    }
    return options_.sessionReadiness.publish();
}

Result<std::unique_ptr<QQuickView>> Pd2ShellRuntime::makePanel(std::string_view outputName,
                                                               QScreen &screenValue) {
    const auto *panelTheme = panel_theme_candidate_ ? panel_theme_candidate_ : theme_.get();
    if (!panelTheme || !panelTheme->current()) {
        return Result<std::unique_ptr<QQuickView>>::failure(surfaceError());
    }
    auto view = std::make_unique<QQuickView>();
    view->setScreen(&screenValue);
    view->setTitle(tr("Prismdrake Panel"));
    view->setColor(Qt::transparent);
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setInitialProperties(
        {{QStringLiteral("themeGeneration"), objectVariant(panelTheme->current())},
         {QStringLiteral("taskModel"), objectVariant(&task_model_)},
         {QStringLiteral("groupedTaskModel"), objectVariant(&grouped_tasks_)},
         {QStringLiteral("outputName"), QString::fromStdString(std::string(outputName))},
         {QStringLiteral("productionAffordancesVisible"), true},
         {QStringLiteral("notificationAffordanceVisible"), false},
         {QStringLiteral("developmentDiagnosticsVisible"), false}});
    view->setSource(
        QUrl(QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/panel/PanelSurface.qml")));
    if (view->status() != QQuickView::Ready || view->rootObject() == nullptr) {
        return Result<std::unique_ptr<QQuickView>>::failure(surfaceError());
    }
    connect(view->rootObject(), SIGNAL(launcherRequested()), this, SLOT(panelLauncherRequested()));
    connect(view->rootObject(), SIGNAL(notificationCenterRequested(QString)), this,
            SLOT(panelNotificationCenterRequested(QString)));
    connect(view->rootObject(), SIGNAL(quickSettingsRequested(QString)), this,
            SLOT(panelQuickSettingsRequested(QString)));
    connect(view->rootObject(), SIGNAL(actionMenuOpened(QString)), this,
            SLOT(panelActionMenuOpened(QString)));
    connect(view->rootObject(), SIGNAL(actionMenuClosed()), this, SLOT(panelActionMenuClosed()));
    connect(view->rootObject(), SIGNAL(actionMenuDismissedToWindowManager()), this,
            SLOT(panelActionMenuDismissedToWindowManager()));
    return Result<std::unique_ptr<QQuickView>>::success(std::move(view));
}

Result<void> Pd2ShellRuntime::ensureSingletonViews() {
    if (launcher_view_) {
        if (!quick_settings_view_ || !notification_center_view_ || !transient_view_ ||
            !logout_view_) {
            return Result<void>::failure(surfaceError());
        }
        singleton_task_policy_gate_.revalidate();
        const auto themeValue = objectVariant(theme_->current());
        for (auto *view :
             {launcher_view_.get(), quick_settings_view_.get(), notification_center_view_.get(),
              transient_view_.get(), logout_view_.get()}) {
            if (view && view->rootObject()) {
                view->rootObject()->setProperty("themeGeneration", themeValue);
            }
        }
        for (const auto &active : composition_.state().activeSurfaces()) {
            if (active.family == composition::SurfaceFamily::actionMenu) {
                continue;
            }
            auto positioned = position(active.family, active.output.value());
            if (!positioned) {
                return positioned;
            }
        }
        return Result<void>::success();
    }
    const auto create = [this](QString title, QString source, QString property, QObject *model) {
        auto view = std::make_unique<QQuickView>();
        view->setTitle(std::move(title));
        view->setFlags(Qt::Tool | Qt::FramelessWindowHint);
        view->setColor(Qt::transparent);
        view->setResizeMode(QQuickView::SizeViewToRootObject);
        view->setInitialProperties(
            {{QStringLiteral("themeGeneration"), objectVariant(theme_->current())},
             {std::move(property), objectVariant(model)}});
        view->setSource(QUrl(std::move(source)));
        if (view->status() != QQuickView::Ready || view->rootObject() == nullptr) {
            return std::unique_ptr<QQuickView>{};
        }
        view->hide();
        return view;
    };
    launcher_view_ =
        create(tr("Prismdrake Launcher"),
               QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/launcher/LauncherSurface.qml"),
               QStringLiteral("launcherModel"), launcher_->presentationModel());
    quick_settings_view_ = create(
        tr("Prismdrake Quick Settings"),
        QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/quicksettings/QuickSettingsSurface.qml"),
        QStringLiteral("quickSettingsModel"), &quick_settings_);
    notification_center_view_ =
        create(tr("Prismdrake Notification Center"),
               QStringLiteral(
                   "qrc:/qt/qml/org/prismdrake/shell/notifications/NotificationCenterSurface.qml"),
               QStringLiteral("presentation"), &notifications_.presentation());
    transient_view_ =
        create(tr("Prismdrake Notifications"),
               QStringLiteral(
                   "qrc:/qt/qml/org/prismdrake/shell/notifications/TransientNotificationStack.qml"),
               QStringLiteral("presentation"), &notifications_.presentation());
    logout_view_ = std::make_unique<QQuickView>();
    logout_view_->setTitle(tr("Prismdrake Logout Confirmation"));
    logout_view_->setFlags(Qt::Dialog | Qt::FramelessWindowHint);
    logout_view_->setModality(Qt::ApplicationModal);
    logout_view_->setColor(Qt::transparent);
    logout_view_->setResizeMode(QQuickView::SizeViewToRootObject);
    logout_view_->setInitialProperties(
        {{QStringLiteral("themeGeneration"), objectVariant(theme_->current())},
         {QStringLiteral("statusText"), tr("Close the session and all applications?")},
         {QStringLiteral("transactionPending"), false}});
    logout_view_->setSource(QUrl(QStringLiteral(
        "qrc:/qt/qml/org/prismdrake/shell/quicksettings/LogoutConfirmationSurface.qml")));
    if (logout_view_->status() == QQuickView::Ready && logout_view_->rootObject() != nullptr) {
        logout_view_->hide();
    } else {
        logout_view_.reset();
    }
    if (!launcher_view_ || !quick_settings_view_ || !notification_center_view_ ||
        !transient_view_ || !logout_view_) {
        return Result<void>::failure(surfaceError());
    }
    if (!singleton_property_worker_ || next_singleton_task_policy_token_ == 0U) {
        return Result<void>::failure(surfaceError());
    }
    std::vector<x11::WindowId> taskPolicyWindows;
    taskPolicyWindows.reserve(5U);
    for (auto *view :
         {launcher_view_.get(), quick_settings_view_.get(), notification_center_view_.get(),
          transient_view_.get(), logout_view_.get()}) {
        auto nativeWindow = windowId(*view);
        if (!nativeWindow) {
            return Result<void>::failure(nativeWindow.error());
        }
        taskPolicyWindows.push_back(nativeWindow.value());
    }
    auto submitted = submitSingletonTaskPolicy(std::move(taskPolicyWindows));
    if (!submitted) {
        return submitted;
    }
    const auto trackPlacement = [this](QQuickView &view, composition::SurfaceFamily family) {
        const auto reposition = [this, family]() {
            const auto surfaces = composition_.state().activeSurfaces();
            const auto active =
                std::ranges::find(surfaces, family, &composition::ActiveSurface::family);
            if (active == surfaces.end()) {
                return;
            }
            auto positioned = position(family, active->output.value());
            if (!positioned) {
                report("surface size placement", positioned.error());
            }
        };
        connect(&view, &QWindow::widthChanged, this, [reposition](int) { reposition(); });
        connect(&view, &QWindow::heightChanged, this, [reposition](int) { reposition(); });
    };
    trackPlacement(*launcher_view_, composition::SurfaceFamily::launcher);
    trackPlacement(*quick_settings_view_, composition::SurfaceFamily::quickSettings);
    trackPlacement(*notification_center_view_, composition::SurfaceFamily::notificationCenter);
    trackPlacement(*transient_view_, composition::SurfaceFamily::transientNotificationAction);
    trackPlacement(*logout_view_, composition::SurfaceFamily::logoutConfirmation);
    connect(launcher_view_->rootObject(), SIGNAL(searchRequested(QString)), this,
            SLOT(launcherSearchRequested(QString)));
    connect(launcher_view_->rootObject(), SIGNAL(dismissRequested()), this,
            SLOT(launcherDismissRequested()));
    connect(launcher_view_->rootObject(), SIGNAL(actionMenuOpened()), this,
            SLOT(launcherActionMenuOpened()));
    connect(launcher_view_->rootObject(), SIGNAL(actionMenuClosed()), this,
            SLOT(launcherActionMenuClosed()));
    connect(quick_settings_view_->rootObject(), SIGNAL(closeRequested()), this,
            SLOT(quickSettingsDismissRequested()));
    connect(quick_settings_view_->rootObject(), SIGNAL(focusExitForward()), this,
            SLOT(quickSettingsDismissRequested()));
    connect(quick_settings_view_->rootObject(), SIGNAL(focusExitBackward()), this,
            SLOT(quickSettingsDismissRequested()));
    connect(notification_center_view_->rootObject(), SIGNAL(closeRequested()), this,
            SLOT(notificationCenterDismissRequested()));
    connect(notification_center_view_->rootObject(), SIGNAL(focusExitForward()), this,
            SLOT(notificationCenterDismissRequested()));
    connect(notification_center_view_->rootObject(), SIGNAL(focusExitBackward()), this,
            SLOT(notificationCenterDismissRequested()));
    connect(logout_view_->rootObject(), SIGNAL(confirmRequested()), this,
            SLOT(logoutConfirmRequested()));
    connect(logout_view_->rootObject(), SIGNAL(cancelRequested()), this,
            SLOT(logoutCancelRequested()));
    return Result<void>::success();
}

Result<void> Pd2ShellRuntime::submitSingletonTaskPolicy(std::vector<x11::WindowId> windows) {
    if (!singleton_property_worker_ || next_singleton_task_policy_token_ == 0U) {
        return Result<void>::failure(surfaceError());
    }
    const auto publicationToken = next_singleton_task_policy_token_;
    next_singleton_task_policy_token_ =
        publicationToken == std::numeric_limits<std::uint64_t>::max() ? 0U : publicationToken + 1U;
    auto begun = singleton_task_policy_gate_.begin(publicationToken);
    if (!begun) {
        return begun;
    }
    auto submitted =
        singleton_property_worker_->submitUnmappedSkipTaskbar(publicationToken, std::move(windows));
    if (!submitted) {
        static_cast<void>(singleton_task_policy_gate_.complete(publicationToken, false, true));
        return submitted;
    }
    return Result<void>::success();
}

void Pd2ShellRuntime::handleSingletonTaskPolicyPublication(std::uint64_t token,
                                                           Result<void> result) {
    const auto disposition =
        singleton_task_policy_gate_.complete(token, result.hasValue(), settings_available_);
    if (disposition == SingletonTaskPolicyGate::CompletionDisposition::stale) {
        return;
    }
    if (disposition == SingletonTaskPolicyGate::CompletionDisposition::discard) {
        pending_singleton_presentations_.clear();
        launcher_view_.reset();
        quick_settings_view_.reset();
        notification_center_view_.reset();
        transient_view_.reset();
        logout_view_.reset();
        theme_.reset();
        return;
    }
    if (disposition == SingletonTaskPolicyGate::CompletionDisposition::fatal) {
        pending_singleton_presentations_.clear();
        shutdown("singleton task-policy X11 publication", result.error());
        return;
    }
    auto replayed = replayPendingSingletonPresentations();
    if (!replayed) {
        pending_singleton_presentations_.clear();
        shutdown("singleton task-policy presentation", replayed.error());
        return;
    }
    if (singleton_task_policy_gate_.pending()) {
        return;
    }
    reconcileTransientPresentation();
    auto ready = maybePublishReady();
    if (!ready) {
        shutdown("transient notification readiness", ready.error());
    }
}

Result<void> Pd2ShellRuntime::replayPendingSingletonPresentations() {
    if (pending_singleton_presentations_.empty()) {
        return Result<void>::success();
    }
    std::vector<PendingSingletonPresentation> active;
    active.reserve(pending_singleton_presentations_.size());
    std::vector<x11::WindowId> changedWindows;
    for (auto pending : pending_singleton_presentations_) {
        if (pending.family == composition::SurfaceFamily::transientNotificationAction &&
            notifications_.presentation().transientModel()->rowCount() == 0) {
            continue;
        }
        const auto surfaces = composition_.state().activeSurfaces();
        const auto desired =
            std::ranges::find(surfaces, pending.family, &composition::ActiveSurface::family);
        if (desired == surfaces.end()) {
            continue;
        }
        pending.output = desired->output;
        auto *view = surface(pending.family);
        if (!view || view->isVisible()) {
            return Result<void>::failure(surfaceError());
        }
        auto currentWindow = windowId(*view);
        if (!currentWindow) {
            return Result<void>::failure(currentWindow.error());
        }
        if (currentWindow.value() != pending.window) {
            changedWindows.push_back(currentWindow.value());
        }
        pending.window = currentWindow.value();
        active.push_back(std::move(pending));
    }
    pending_singleton_presentations_ = std::move(active);
    if (pending_singleton_presentations_.empty()) {
        return Result<void>::success();
    }

    if (!changedWindows.empty()) {
        return submitSingletonTaskPolicy(std::move(changedWindows));
    }

    auto presentations = std::move(pending_singleton_presentations_);
    pending_singleton_presentations_.clear();
    for (const auto &presentation : presentations) {
        auto presented = presentSingleton(presentation);
        if (!presented) {
            return presented;
        }
    }
    return Result<void>::success();
}

Result<void> Pd2ShellRuntime::presentSingleton(const PendingSingletonPresentation &presentation) {
    auto *view = surface(presentation.family);
    if (!view) {
        return Result<void>::failure(surfaceError());
    }
    const bool wasVisible = view->isVisible();
    auto positioned = position(presentation.family, presentation.output.value());
    if (!positioned) {
        return positioned;
    }
    view->show();
    view->raise();
    if (!presentation.focus) {
        return Result<void>::success();
    }
    view->requestActivate();
    if (!shouldInitializeSingletonControlFocus(wasVisible, presentation.focus)) {
        return Result<void>::success();
    }
    const char *method = "focusFirstControl";
    if (presentation.family == composition::SurfaceFamily::launcher) {
        method = "focusSearch";
    } else if (presentation.family == composition::SurfaceFamily::transientNotificationAction) {
        method = "focusFirstCard";
    }
    QMetaObject::invokeMethod(view->rootObject(), method, Qt::QueuedConnection);
    return Result<void>::success();
}

void Pd2ShellRuntime::reconcileTransientPresentation() {
    if (!singleton_task_policy_gate_.ready()) {
        return;
    }
    const bool hasTransient = notifications_.presentation().transientModel()->rowCount() > 0;
    if (hasTransient && settings_available_ && surface_snapshot_ &&
        surface_snapshot_->primaryOutputName()) {
        static_cast<void>(composition_.present(
            composition::SurfaceFamily::transientNotificationAction,
            *surface_snapshot_->primaryOutputName(), composition::FocusOrigin::windowManager()));
        return;
    }
    static_cast<void>(
        composition_.dismiss(composition::SurfaceFamily::transientNotificationAction));
}

QQuickView *Pd2ShellRuntime::surface(composition::SurfaceFamily family) const noexcept {
    switch (family) {
    case composition::SurfaceFamily::launcher:
        return launcher_view_.get();
    case composition::SurfaceFamily::quickSettings:
        return quick_settings_view_.get();
    case composition::SurfaceFamily::notificationCenter:
        return notification_center_view_.get();
    case composition::SurfaceFamily::transientNotificationAction:
        return transient_view_.get();
    case composition::SurfaceFamily::logoutConfirmation:
        return logout_view_.get();
    case composition::SurfaceFamily::actionMenu:
        return nullptr;
    }
    return nullptr;
}

Result<void> Pd2ShellRuntime::position(composition::SurfaceFamily family,
                                       std::string_view outputName) {
    auto *view = surface(family);
    auto *outputScreen = screen(outputName);
    if (!view || !outputScreen) {
        return Result<void>::failure(surfaceError());
    }
    const auto area = outputScreen->availableGeometry();
    const auto width = std::min(view->width(), area.width());
    const auto height = std::min(view->height(), area.height());
    const bool left = family == composition::SurfaceFamily::launcher;
    view->setGeometry(left ? area.left() : area.right() - width + 1, area.bottom() - height + 1,
                      width, height);
    return Result<void>::success();
}

control::ShellRequestFailure Pd2ShellRuntime::validatePresentation(
    std::span<const composition::CompositionIntent> intents) const {
    for (const auto &intent : intents) {
        const auto failure = std::visit(
            [this](const auto &value) -> control::ShellRequestFailure {
                using Intent = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Intent, composition::PresentSurfaceIntent> ||
                              std::is_same_v<Intent, composition::FocusSurfaceIntent>) {
                    if (value.family == composition::SurfaceFamily::actionMenu) {
                        return screen(value.output.value()) == nullptr
                                   ? control::ShellRequestFailure::output_unavailable
                                   : control::ShellRequestFailure::none;
                    }
                    if (!singleton_task_policy_gate_.ready()) {
                        const auto pending =
                            std::ranges::find(pending_singleton_presentations_, value.family,
                                              &PendingSingletonPresentation::family);
                        if (pending == pending_singleton_presentations_.end()) {
                            return control::ShellRequestFailure::busy;
                        }
                    }
                    if (surface(value.family) == nullptr) {
                        return control::ShellRequestFailure::internal;
                    }
                    return screen(value.output.value()) == nullptr
                               ? control::ShellRequestFailure::output_unavailable
                               : control::ShellRequestFailure::none;
                } else if constexpr (std::is_same_v<Intent, composition::MoveSurfaceIntent>) {
                    if (value.family != composition::SurfaceFamily::actionMenu &&
                        surface(value.family) == nullptr) {
                        return control::ShellRequestFailure::internal;
                    }
                    return screen(value.to.value()) == nullptr
                               ? control::ShellRequestFailure::output_unavailable
                               : control::ShellRequestFailure::none;
                } else if constexpr (std::is_same_v<Intent,
                                                    composition::RestoreControlFocusIntent>) {
                    if (!value.origin.output() || !value.origin.role()) {
                        return control::ShellRequestFailure::internal;
                    }
                    if (*value.origin.role() == composition::FocusControlRole::launcherResult) {
                        return launcher_view_ ? control::ShellRequestFailure::none
                                              : control::ShellRequestFailure::internal;
                    }
                    if (!panels_ || panels_->view(value.origin.output()->value()) == nullptr) {
                        return control::ShellRequestFailure::output_unavailable;
                    }
                } else if constexpr (std::is_same_v<
                                         Intent, composition::ReleaseFocusToWindowManagerIntent>) {
                    return panels_ ? control::ShellRequestFailure::none
                                   : control::ShellRequestFailure::internal;
                } else if constexpr (std::is_same_v<Intent, composition::CancelLogoutIntent>) {
                    return logout_transaction_ ? control::ShellRequestFailure::none
                                               : control::ShellRequestFailure::busy;
                }
                return control::ShellRequestFailure::none;
            },
            intent);
        if (failure != control::ShellRequestFailure::none) {
            return failure;
        }
    }
    return control::ShellRequestFailure::none;
}

control::ShellRequestFailure
Pd2ShellRuntime::apply(std::span<const composition::CompositionIntent> intents) {
    const auto validation = validatePresentation(intents);
    if (validation != control::ShellRequestFailure::none) {
        return validation;
    }

    std::vector<PendingSingletonPresentation> deferred;
    bool invalidDeferredPresentation = false;
    for (const auto &intent : intents) {
        std::visit(
            [this, &deferred, &invalidDeferredPresentation](const auto &value) {
                using Intent = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Intent, composition::PresentSurfaceIntent> ||
                              std::is_same_v<Intent, composition::FocusSurfaceIntent>) {
                    if (value.family == composition::SurfaceFamily::actionMenu) {
                        return;
                    }
                    auto *view = surface(value.family);
                    if (!view || view->isVisible()) {
                        return;
                    }
                    const auto pending =
                        std::ranges::find(pending_singleton_presentations_, value.family,
                                          &PendingSingletonPresentation::family);
                    if (pending != pending_singleton_presentations_.end()) {
                        pending->output = value.output;
                        if constexpr (std::is_same_v<Intent, composition::FocusSurfaceIntent>) {
                            pending->focus = true;
                        }
                        return;
                    }
                    const auto existing = std::ranges::find(deferred, value.family,
                                                            &PendingSingletonPresentation::family);
                    if (existing != deferred.end()) {
                        existing->output = value.output;
                        if constexpr (std::is_same_v<Intent, composition::FocusSurfaceIntent>) {
                            existing->focus = true;
                        }
                        return;
                    }
                    view->destroy();
                    auto positioned = position(value.family, value.output.value());
                    if (!positioned) {
                        invalidDeferredPresentation = true;
                        return;
                    }
                    auto nativeWindow = windowId(*view);
                    if (!nativeWindow) {
                        invalidDeferredPresentation = true;
                        return;
                    }
                    deferred.push_back({value.family, value.output, nativeWindow.value(),
                                        std::is_same_v<Intent, composition::FocusSurfaceIntent>});
                }
            },
            intent);
    }
    if (invalidDeferredPresentation) {
        return control::ShellRequestFailure::internal;
    }
    if (!deferred.empty()) {
        std::vector<x11::WindowId> windows;
        windows.reserve(deferred.size());
        for (const auto &presentation : deferred) {
            windows.push_back(presentation.window);
        }
        pending_singleton_presentations_ = std::move(deferred);
        auto submitted = submitSingletonTaskPolicy(std::move(windows));
        if (!submitted) {
            pending_singleton_presentations_.clear();
            shutdown("singleton task-policy submission", submitted.error());
            return control::ShellRequestFailure::internal;
        }
    }

    for (const auto &intent : intents) {
        const auto failure = std::visit(
            [this](const auto &value) -> control::ShellRequestFailure {
                using Intent = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Intent, composition::PresentSurfaceIntent> ||
                              std::is_same_v<Intent, composition::FocusSurfaceIntent>) {
                    if (value.family == composition::SurfaceFamily::actionMenu) {
                        return control::ShellRequestFailure::none;
                    }
                    auto *view = surface(value.family);
                    if (!view) {
                        return control::ShellRequestFailure::internal;
                    }
                    const auto deferred =
                        std::ranges::find(pending_singleton_presentations_, value.family,
                                          &PendingSingletonPresentation::family);
                    if (deferred == pending_singleton_presentations_.end()) {
                        auto nativeWindow = windowId(*view);
                        if (!nativeWindow) {
                            return control::ShellRequestFailure::internal;
                        }
                        auto presented = presentSingleton(
                            {value.family, value.output, nativeWindow.value(),
                             std::is_same_v<Intent, composition::FocusSurfaceIntent>});
                        if (!presented) {
                            return control::ShellRequestFailure::internal;
                        }
                    }
                } else if constexpr (std::is_same_v<Intent, composition::MoveSurfaceIntent>) {
                    if (value.family != composition::SurfaceFamily::actionMenu) {
                        static_cast<void>(position(value.family, value.to.value()));
                        const auto pending =
                            std::ranges::find(pending_singleton_presentations_, value.family,
                                              &PendingSingletonPresentation::family);
                        if (pending != pending_singleton_presentations_.end()) {
                            pending->output = value.to;
                        }
                    }
                } else if constexpr (std::is_same_v<Intent, composition::DismissSurfaceIntent>) {
                    if (auto *view = surface(value.family)) {
                        view->hide();
                    }
                } else if constexpr (std::is_same_v<Intent,
                                                    composition::RestoreControlFocusIntent>) {
                    return restoreControlFocus(value.origin);
                } else if constexpr (std::is_same_v<
                                         Intent, composition::ReleaseFocusToWindowManagerIntent>) {
                    auto released = panels_->releaseKeyboardAccess();
                    if (!released) {
                        return control::ShellRequestFailure::internal;
                    }
                } else if constexpr (std::is_same_v<Intent, composition::CancelLogoutIntent>) {
                    const auto cancelled = session_control_.cancelLogout(*logout_transaction_);
                    return cancelled.accepted() ? control::ShellRequestFailure::none
                                                : control::ShellRequestFailure::busy;
                } else if constexpr (std::is_same_v<Intent,
                                                    composition::DependencyAvailabilityIntent>) {
                    quick_settings_.setNotificationCenterAvailable(value.available);
                }
                return control::ShellRequestFailure::none;
            },
            intent);
        if (failure != control::ShellRequestFailure::none) {
            return failure;
        }
    }
    return control::ShellRequestFailure::none;
}

control::ShellRequestFailure Pd2ShellRuntime::focusPanel(const composition::OutputName &output) {
    auto access = panels_->requestKeyboardAccess(output.value(), [this](QQuickView &view) {
        if (!view.rootObject() ||
            !QMetaObject::invokeMethod(view.rootObject(), "focusLauncher", Qt::QueuedConnection)) {
            report("panel focus", surfaceError());
        }
    });
    if (!access) {
        return control::ShellRequestFailure::output_unavailable;
    }
    return control::ShellRequestFailure::none;
}

control::ShellRequestFailure Pd2ShellRuntime::releasePanelKeyboardAccess() {
    return panels_->releaseKeyboardAccess() ? control::ShellRequestFailure::none
                                            : control::ShellRequestFailure::internal;
}

QScreen *Pd2ShellRuntime::screen(std::string_view outputName) {
    const auto screens = QGuiApplication::screens();
    const auto found = std::ranges::find_if(screens, [outputName](QScreen *value) {
        return value != nullptr && value->name().toStdString() == outputName;
    });
    return found == screens.end() ? nullptr : *found;
}

void Pd2ShellRuntime::panelLauncherRequested() {
    const auto outputValue = sender() ? sender()->property("outputName").toString() : QString{};
    auto output = composition::OutputName::create(outputValue.toStdString());
    auto id = composition::FocusControlId::create("launcher-button");
    if (!output) {
        report("panel launcher origin", output.error());
        return;
    }
    if (!id) {
        report("panel launcher origin", id.error());
        return;
    }
    auto origin = composition::FocusOrigin::control(
        output.value(), composition::FocusControlRole::panelLauncher, id.value());
    if (!origin) {
        report("panel launcher origin", origin.error());
        return;
    }
    auto toggled = composition_.toggle(composition::SurfaceFamily::launcher,
                                       outputValue.toStdString(), std::move(origin).value());
    if (!toggled) {
        report("panel launcher", toggled.error());
    }
}

void Pd2ShellRuntime::panelNotificationCenterRequested(const QString &outputName) {
    auto output = composition::OutputName::create(outputName.toStdString());
    auto id = composition::FocusControlId::create("notification-center-button");
    if (!output) {
        report("panel notification center origin", output.error());
        return;
    }
    if (!id) {
        report("panel notification center origin", id.error());
        return;
    }
    auto origin = composition::FocusOrigin::control(
        output.value(), composition::FocusControlRole::panelNotificationCenter, id.value());
    if (!origin) {
        report("panel notification center origin", origin.error());
        return;
    }
    auto presented = composition_.present(composition::SurfaceFamily::notificationCenter,
                                          outputName.toStdString(), std::move(origin).value());
    if (!presented) {
        report("panel notification center", presented.error());
    }
}

void Pd2ShellRuntime::panelQuickSettingsRequested(const QString &outputName) {
    auto output = composition::OutputName::create(outputName.toStdString());
    auto id = composition::FocusControlId::create("quick-settings-button");
    if (!output) {
        report("panel quick settings origin", output.error());
        return;
    }
    if (!id) {
        report("panel quick settings origin", id.error());
        return;
    }
    auto origin = composition::FocusOrigin::control(
        output.value(), composition::FocusControlRole::panelQuickSettings, id.value());
    if (!origin) {
        report("panel quick settings origin", origin.error());
        return;
    }
    auto presented = composition_.present(composition::SurfaceFamily::quickSettings,
                                          outputName.toStdString(), std::move(origin).value());
    if (!presented) {
        report("panel quick settings", presented.error());
    }
}

Result<void> Pd2ShellRuntime::presentActionMenu(std::string_view outputName,
                                                composition::FocusControlRole role,
                                                std::string_view controlId) {
    auto output = composition::OutputName::create(std::string(outputName));
    auto id = composition::FocusControlId::create(std::string(controlId));
    if (!output) {
        return Result<void>::failure(output.error());
    }
    if (!id) {
        return Result<void>::failure(id.error());
    }
    auto origin = composition::FocusOrigin::control(output.value(), role, id.value());
    if (!origin) {
        return Result<void>::failure(origin.error());
    }
    auto presented = composition_.present(composition::SurfaceFamily::actionMenu, outputName,
                                          std::move(origin).value());
    if (presented) {
        action_menu_role_ = role;
    }
    return presented;
}

void Pd2ShellRuntime::dismissActionMenu() {
    auto dismissed = composition_.dismiss(composition::SurfaceFamily::actionMenu);
    if (!dismissed) {
        report("action menu dismissal", dismissed.error());
        return;
    }
    action_menu_role_.reset();
}

void Pd2ShellRuntime::dismissActionMenuToWindowManager() {
    auto dismissed = composition_.dismissToWindowManager(composition::SurfaceFamily::actionMenu);
    if (!dismissed) {
        report("action menu dismissal", dismissed.error());
        return;
    }
    action_menu_role_.reset();
}

Result<void> Pd2ShellRuntime::replacePin(const prismdrake::launcher::DesktopFileId &desktopFileId,
                                         bool pinned) {
    if (!pin_state_ || !pin_state_->current()) {
        return Result<void>::failure(surfaceError());
    }
    auto replacement = pinned
                           ? panel::withPinnedApplication(*pin_state_->current(), desktopFileId)
                           : panel::withoutPinnedApplication(*pin_state_->current(), desktopFileId);
    if (!replacement) {
        return Result<void>::failure(replacement.error());
    }
    return pin_state_->replace(std::move(replacement).value());
}

Result<void> Pd2ShellRuntime::reconcileGroupedTasks() {
    const auto tasks = tasks_ ? tasks_->currentSnapshot() : nullptr;
    const auto &catalog = launcher_->catalog();
    if (!tasks || !catalog || !catalog->complete || !pin_state_ || !pin_state_->current() ||
        !settings_snapshot_ || !surface_snapshot_ || !theme_ || !theme_->current()) {
        return Result<void>::success();
    }
    if (association_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure(surfaceError());
    }

    std::vector<prismdrake::launcher::CatalogApplicationIdentity> identities;
    identities.reserve(catalog->eligibleEntryIndices.size());
    for (const auto index : catalog->eligibleEntryIndices) {
        if (index >= catalog->discovery->entries.size()) {
            return Result<void>::failure(surfaceError());
        }
        const auto &entry = catalog->discovery->entries[index];
        identities.push_back({entry.id, entry.entry.startupWmClass});
    }
    std::vector<prismdrake::launcher::WindowAssociationObservation> windows;
    windows.reserve(tasks->tasks().size());
    for (const auto &task : tasks->tasks()) {
        auto window = prismdrake::launcher::ObservedWindowIdentity::fromObservation(
            task.window().value(), task.incarnation().value());
        if (!window) {
            return Result<void>::failure(window.error());
        }
        windows.push_back({std::move(window).value(), std::nullopt, std::nullopt,
                           task.applicationId().empty()
                               ? std::nullopt
                               : std::optional<std::string>{task.applicationId()}});
    }
    auto associations =
        prismdrake::launcher::associateApplications(identities, windows, ++association_generation_);
    if (!associations) {
        return Result<void>::failure(associations.error());
    }
    double availableWidth = std::numeric_limits<double>::max();
    for (const auto &panel : surface_snapshot_->panels()) {
        availableWidth =
            std::min(availableWidth,
                     std::max(0.0, static_cast<double>(panel.logicalGeometry.width) - 320.0));
    }
    if (availableWidth == std::numeric_limits<double>::max()) {
        availableWidth = 0.0;
    }
    auto grouped = panel::buildGroupedTasks(
        {tasks, std::move(associations).value(), catalog, pin_state_->current(),
         settings_snapshot_->candidate.configuration.panel.grouping, availableWidth,
         theme_->current()->panel()->minimumTargetSize(), 4.0,
         settings_snapshot_->generation.value(), surface_snapshot_->surfaceGeneration()});
    if (!grouped) {
        return Result<void>::failure(grouped.error());
    }
    return grouped_tasks_.applySnapshot(std::move(grouped).value());
}

void Pd2ShellRuntime::panelActionMenuOpened(const QString &outputName) {
    auto presented =
        presentActionMenu(outputName.toStdString(), composition::FocusControlRole::panelTaskGroup,
                          "panel-task-strip");
    if (!presented) {
        if (auto *panel = panels_->view(outputName.toStdString()); panel && panel->rootObject()) {
            QMetaObject::invokeMethod(panel->rootObject(), "closeTaskActionMenus",
                                      Qt::QueuedConnection);
        }
    }
}

void Pd2ShellRuntime::panelActionMenuClosed() {
    if (action_menu_role_ == composition::FocusControlRole::panelTaskGroup) {
        dismissActionMenu();
    }
}

void Pd2ShellRuntime::panelActionMenuDismissedToWindowManager() {
    if (action_menu_role_ == composition::FocusControlRole::panelTaskGroup) {
        dismissActionMenuToWindowManager();
    }
}

void Pd2ShellRuntime::launcherSearchRequested(const QString &query) {
    static_cast<void>(launcher_->setSearchQuery(query));
}

void Pd2ShellRuntime::launcherDismissRequested() {
    static_cast<void>(composition_.dismiss(composition::SurfaceFamily::launcher));
}

void Pd2ShellRuntime::launcherActionMenuOpened() {
    const auto surfaces = composition_.state().activeSurfaces();
    const auto active = std::ranges::find(surfaces, composition::SurfaceFamily::launcher,
                                          &composition::ActiveSurface::family);
    if (active == surfaces.end()) {
        QMetaObject::invokeMethod(launcher_view_->rootObject(), "closeActionMenus",
                                  Qt::QueuedConnection);
        return;
    }
    auto presented = presentActionMenu(
        active->output.value(), composition::FocusControlRole::launcherResult, "launcher-result");
    if (!presented) {
        QMetaObject::invokeMethod(launcher_view_->rootObject(), "closeActionMenus",
                                  Qt::QueuedConnection);
    }
}

void Pd2ShellRuntime::launcherActionMenuClosed() {
    if (action_menu_role_ == composition::FocusControlRole::launcherResult) {
        dismissActionMenu();
    }
}

control::ShellRequestFailure
Pd2ShellRuntime::restoreControlFocus(const composition::FocusOrigin &origin) {
    if (!origin.output() || !origin.role()) {
        return control::ShellRequestFailure::internal;
    }
    if (*origin.role() == composition::FocusControlRole::launcherResult) {
        if (!launcher_view_ || !launcher_view_->rootObject() ||
            !QMetaObject::invokeMethod(launcher_view_->rootObject(), "restoreActionMenuFocus",
                                       Qt::QueuedConnection)) {
            return control::ShellRequestFailure::internal;
        }
        return control::ShellRequestFailure::none;
    }
    if (*origin.role() == composition::FocusControlRole::panelLauncher) {
        return focusPanel(*origin.output());
    }
    const char *method = nullptr;
    if (*origin.role() == composition::FocusControlRole::panelTaskGroup) {
        method = "restoreActionMenuFocus";
    } else if (*origin.role() == composition::FocusControlRole::panelQuickSettings) {
        method = "focusQuickSettings";
    } else if (*origin.role() == composition::FocusControlRole::panelNotificationCenter) {
        method = "focusNotification";
    } else {
        return control::ShellRequestFailure::internal;
    }
    auto access =
        panels_->requestKeyboardAccess(origin.output()->value(), [this, method](QQuickView &view) {
            if (!view.rootObject() ||
                !QMetaObject::invokeMethod(view.rootObject(), method, Qt::QueuedConnection)) {
                report("panel focus restore", surfaceError());
            }
        });
    return access ? control::ShellRequestFailure::none
                  : control::ShellRequestFailure::output_unavailable;
}

void Pd2ShellRuntime::quickSettingsDismissRequested() {
    static_cast<void>(composition_.dismiss(composition::SurfaceFamily::quickSettings));
}

void Pd2ShellRuntime::notificationCenterDismissRequested() {
    static_cast<void>(composition_.dismiss(composition::SurfaceFamily::notificationCenter));
}

void Pd2ShellRuntime::logoutConfirmRequested() {
    if (!logout_transaction_) {
        return;
    }
    const auto confirmed = session_control_.confirmLogout(*logout_transaction_);
    if (confirmed.accepted() && logout_view_ && logout_view_->rootObject()) {
        logout_view_->rootObject()->setProperty("transactionPending", true);
        logout_view_->rootObject()->setProperty("statusText", tr("Closing the session…"));
    }
}

void Pd2ShellRuntime::logoutCancelRequested() {
    if (!logout_transaction_) {
        return;
    }
    const auto cancelled = session_control_.cancelLogout(*logout_transaction_);
    if (cancelled.accepted() && logout_view_ && logout_view_->rootObject()) {
        logout_view_->rootObject()->setProperty("transactionPending", true);
        logout_view_->rootObject()->setProperty("statusText", tr("Cancelling logout…"));
    }
}

void Pd2ShellRuntime::handleSessionState(controlclients::SessionStatePublication publication) {
    using controlclients::SessionState;
    if (publication.state == SessionState::unavailable) {
        logout_transaction_.reset();
        quick_settings_.setSessionAvailable(false);
        static_cast<void>(composition_.dismiss(composition::SurfaceFamily::logoutConfirmation));
        return;
    }

    quick_settings_.setSessionAvailable(true);
    switch (publication.state) {
    case SessionState::confirmationRequested:
        if (!publication.transaction || !surface_snapshot_ ||
            !surface_snapshot_->primaryOutputName()) {
            logout_transaction_.reset();
            quick_settings_.observeSessionState(quicksettings::SessionPresentationState::failed);
            return;
        }
        logout_transaction_ = publication.transaction;
        quick_settings_.observeSessionState(
            quicksettings::SessionPresentationState::confirmationRequested);
        if (logout_view_ && logout_view_->rootObject()) {
            logout_view_->rootObject()->setProperty("transactionPending", false);
            logout_view_->rootObject()->setProperty("statusText",
                                                    tr("Close the session and all applications?"));
        }
        static_cast<void>(composition_.present(composition::SurfaceFamily::logoutConfirmation,
                                               *surface_snapshot_->primaryOutputName(),
                                               composition::FocusOrigin::windowManager()));
        break;
    case SessionState::confirmed:
    case SessionState::stoppingShell:
    case SessionState::stoppingNotifyd:
    case SessionState::stoppingSettingsd:
        logout_transaction_ = publication.transaction;
        quick_settings_.observeSessionState(quicksettings::SessionPresentationState::stopping);
        if (logout_view_ && logout_view_->rootObject()) {
            logout_view_->rootObject()->setProperty("transactionPending", true);
            logout_view_->rootObject()->setProperty("statusText", tr("Closing the session…"));
        }
        break;
    case SessionState::failed:
        logout_transaction_.reset();
        quick_settings_.observeSessionState(quicksettings::SessionPresentationState::failed);
        static_cast<void>(composition_.dismiss(composition::SurfaceFamily::logoutConfirmation));
        break;
    case SessionState::idle:
    case SessionState::cancelled:
    case SessionState::complete:
        logout_transaction_.reset();
        quick_settings_.observeSessionState(quicksettings::SessionPresentationState::idle);
        static_cast<void>(composition_.dismiss(composition::SurfaceFamily::logoutConfirmation));
        break;
    case SessionState::unavailable:
        break;
    }
}

void Pd2ShellRuntime::report(const char *context, const foundation::Error &error) {
    qWarning().noquote() << "prismdrake-shell:" << context << '-'
                         << QString::fromStdString(error.message)
                         << QString::fromStdString(error.recovery);
}

void Pd2ShellRuntime::shutdown(const char *context, const foundation::Error &error) {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    report(context, error);
    QCoreApplication::exit(foundation::processExitCode(foundation::exitStatusFor(error)));
}

} // namespace prismdrake::shell::runtime
