#include "ShellRuntime.hpp"

#include "ExitStatus.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickView>
#include <QRect>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>

namespace prismdrake::shell::runtime {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Error surfaceError() {
    return {ErrorCode::invalid_environment, "A required shell QML surface could not be loaded.",
            "Install the matching Prismdrake shell QML modules and restart the shell."};
}

[[nodiscard]] Error connectionError() {
    return {ErrorCode::invalid_environment,
            "A required shell surface signal could not be connected.",
            "Install matching Prismdrake shell runtime and QML module versions."};
}

[[nodiscard]] Error missingPresentationError() {
    return {ErrorCode::cancelled, "The shell presentation epoch is unavailable.",
            "Wait for one complete settings-owner snapshot before opening shell surfaces."};
}

[[nodiscard]] Error invalidPanelHeight() {
    return {ErrorCode::validation_error, "The projected panel height is not representable.",
            "Use a bounded positive panel height in the complete theme snapshot."};
}

[[nodiscard]] QVariant objectVariant(QObject *object) { return QVariant::fromValue(object); }

} // namespace

ShellRuntime::ShellRuntime(ShellRuntimeOptions options)
    : options_(std::move(options)), task_model_(this), settings_client_(this) {}

ShellRuntime::~ShellRuntime() {
    destroyPresentationEpoch();
    settings_client_.stop();
}

Result<std::unique_ptr<ShellRuntime>> ShellRuntime::create(ShellRuntimeOptions options) {
    std::unique_ptr<ShellRuntime> runtime;
    try {
        runtime = std::unique_ptr<ShellRuntime>(new ShellRuntime(std::move(options)));
    } catch (const std::bad_alloc &) {
        return Result<std::unique_ptr<ShellRuntime>>::failure(
            {ErrorCode::too_large, "The shell runtime could not be allocated.",
             "Reduce memory pressure before restarting prismdrake-shell."});
    }
    auto initialized = runtime->initialize();
    if (!initialized) {
        return Result<std::unique_ptr<ShellRuntime>>::failure(initialized.error());
    }
    return Result<std::unique_ptr<ShellRuntime>>::success(std::move(runtime));
}

Result<void> ShellRuntime::initialize() {
    auto launcher = launcher::controller::LauncherController::create(std::move(options_.launcher));
    if (!launcher) {
        return Result<void>::failure(launcher.error());
    }
    launcher_controller_ = std::move(launcher).value();

    auto tasks = taskcontroller::TaskController::create(
        task_model_, options_.display, [this](const Error &error) { handleX11Loss(error); },
        [this](const Error &error) { reportRecoverable("task refresh", error); },
        [this](const taskcontroller::TaskRequestUpdate &update) {
            if (update.outcome == x11::TaskRequestOutcome::deliveryRejected ||
                update.outcome == x11::TaskRequestOutcome::refused ||
                update.outcome == x11::TaskRequestOutcome::targetReplaced) {
                reportRecoverable(
                    "task request",
                    {ErrorCode::cancelled, "The window manager did not apply the task request.",
                     "Use the current task entry and retry after the next authoritative refresh."});
            }
        });
    if (!tasks) {
        return Result<void>::failure(tasks.error());
    }
    task_controller_ = std::move(tasks).value();

    connect(&settings_client_, &settings::SettingsSnapshotClient::snapshotChanged, this,
            &ShellRuntime::handleSettingsSnapshotChanged);
    connect(&settings_client_, &settings::SettingsSnapshotClient::stateChanged, this,
            &ShellRuntime::handleSettingsStateChanged);
    connect(
        launcher_controller_.get(), &launcher::controller::LauncherController::catalogRefreshFailed,
        this, [this]() {
            reportRecoverable(
                "launcher catalog",
                {ErrorCode::io_error, "The application catalog refresh failed.",
                 "The launcher retained a bounded error state; retry after data is available."});
        });
    connect(launcher_controller_.get(), &launcher::controller::LauncherController::searchFailed,
            this, [this]() {
                reportRecoverable(
                    "launcher search",
                    {ErrorCode::validation_error, "The application search request failed.",
                     "Refine the bounded query or retry after the catalog refreshes."});
            });
    connect(launcher_controller_.get(), &launcher::controller::LauncherController::launchRejected,
            this, [this]() {
                reportRecoverable(
                    "application launch",
                    {ErrorCode::cancelled, "The application launch request was rejected.",
                     "Select a current launcher result and retry once no launch is pending."});
            });
    connect(launcher_controller_.get(), &launcher::controller::LauncherController::launchFailed,
            this, [this]() {
                reportRecoverable(
                    "application launch",
                    {ErrorCode::io_error, "The application could not be started.",
                     "Review the desktop entry and executable availability, then retry."});
            });
    connect(launcher_controller_.get(), &launcher::controller::LauncherController::launchCompleted,
            this, &ShellRuntime::dismissLauncherWithoutPanelFocus);
    return Result<void>::success();
}

Result<void> ShellRuntime::start() {
    if (started_) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The shell runtime is already started.",
                                      "Start each prismdrake-shell runtime only once."});
    }
    auto settingsStarted = settings_client_.start();
    if (!settingsStarted) {
        return settingsStarted;
    }
    auto refresh = launcher_controller_->refresh();
    if (!refresh) {
        settings_client_.stop();
        return refresh;
    }
    started_ = true;
    return Result<void>::success();
}

void ShellRuntime::handleSettingsSnapshotChanged() {
    const auto snapshot = settings_client_.currentSnapshot();
    if (!snapshot) {
        auto actions = state_.loseSettingsOwner();
        auto applied = executeActions(actions);
        if (!applied) {
            reportRecoverable("settings owner loss", applied.error());
        }
        return;
    }
    auto applied = applySnapshot(snapshot);
    if (!applied) {
        if (!state_.presentationAvailable()) {
            requestShutdown("initial settings snapshot", applied.error());
        } else {
            reportRecoverable("settings snapshot", applied.error());
        }
    }
}

void ShellRuntime::handleSettingsStateChanged() {
    if (settings_client_.state() != settings::SettingsSnapshotClient::State::failed ||
        !settings_client_.lastError()) {
        return;
    }
    const auto &error = *settings_client_.lastError();
    if (error.code == ErrorCode::io_error) {
        requestShutdown("settings bus", error);
    } else {
        reportRecoverable("settings service", error);
    }
}

Result<void> ShellRuntime::applySnapshot(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    if (!snapshot) {
        return Result<void>::failure(missingPresentationError());
    }
    auto actions = state_.acceptSettingsSnapshot();
    auto applied = executeActions(actions, std::move(snapshot));
    if (!applied &&
        std::ranges::find(actions, RuntimeAction::createPresentationEpoch) != actions.end()) {
        static_cast<void>(state_.loseSettingsOwner());
        destroyPresentationEpoch();
    }
    return applied;
}

Result<void> ShellRuntime::executeActions(
    std::span<const RuntimeAction> actions,
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    for (const auto action : actions) {
        switch (action) {
        case RuntimeAction::createPresentationEpoch: {
            auto created = createPresentationEpoch(snapshot);
            if (!created) {
                return created;
            }
            break;
        }
        case RuntimeAction::updatePresentationEpoch: {
            auto updated = updatePresentationEpoch(snapshot);
            if (!updated) {
                return updated;
            }
            break;
        }
        case RuntimeAction::destroyPresentationEpoch:
            destroyPresentationEpoch();
            break;
        case RuntimeAction::showLauncher:
            if (!launcher_view_) {
                return Result<void>::failure(missingPresentationError());
            }
            positionLauncher();
            launcher_was_active_ = false;
            launcher_view_->show();
            launcher_view_->raise();
            launcher_view_->requestActivate();
            break;
        case RuntimeAction::hideLauncher:
            if (launcher_view_) {
                launcher_view_->hide();
            }
            launcher_was_active_ = false;
            break;
        case RuntimeAction::requestLauncherFocus:
            if (launcher_view_ && launcher_view_->rootObject()) {
                QMetaObject::invokeMethod(launcher_view_->rootObject(), "focusSearch",
                                          Qt::QueuedConnection);
            }
            break;
        case RuntimeAction::requestPanelKeyboardAccess:
            if (!panel_host_) {
                return Result<void>::failure(missingPresentationError());
            }
            if (auto access = panel_host_->requestKeyboardAccess(); !access) {
                return access;
            }
            break;
        case RuntimeAction::releasePanelKeyboardAccess:
            if (panel_host_) {
                if (auto released = panel_host_->releaseKeyboardAccess(); !released) {
                    return released;
                }
            }
            break;
        case RuntimeAction::requestPanelLauncherFocus:
            if (panel_view_ && panel_view_->rootObject()) {
                QMetaObject::invokeMethod(panel_view_->rootObject(), "focusLauncher",
                                          Qt::QueuedConnection);
            }
            break;
        case RuntimeAction::requestShutdown:
            break;
        }
    }
    return Result<void>::success();
}

Result<void> ShellRuntime::createPresentationEpoch(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    destroyPresentationEpoch();
    theme_adapter_ = std::make_unique<theme::ShellThemeSnapshotAdapter>(this);
    auto projected = theme_adapter_->applySnapshot(std::move(snapshot));
    if (!projected) {
        destroyPresentationEpoch();
        return projected;
    }
    auto views = createViews();
    if (!views) {
        destroyPresentationEpoch();
        return views;
    }
    auto height = projectedPanelHeight();
    if (!height) {
        destroyPresentationEpoch();
        return Result<void>::failure(height.error());
    }
    auto host = window::PanelWindowHost::create(
        *panel_view_, options_.display, height.value(),
        [this](const Error &error) { handleX11Loss(error); },
        [this](const Error &error) { reportRecoverable("panel window", error); });
    if (!host) {
        destroyPresentationEpoch();
        return Result<void>::failure(host.error());
    }
    panel_host_ = std::move(host).value();
    positionLauncher();
    return Result<void>::success();
}

Result<void> ShellRuntime::updatePresentationEpoch(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    if (!theme_adapter_ || !panel_host_) {
        return Result<void>::failure(missingPresentationError());
    }

    theme::ShellThemeSnapshotAdapter candidate;
    auto validated = candidate.applySnapshot(snapshot);
    if (!validated) {
        return validated;
    }
    const auto candidateHeight = candidate.current()->panel()->panelHeight();
    if (!std::isfinite(candidateHeight) || candidateHeight <= 0.0 ||
        candidateHeight > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return Result<void>::failure(invalidPanelHeight());
    }
    const auto nextHeight = static_cast<std::uint32_t>(std::ceil(candidateHeight));
    auto previousHeight = projectedPanelHeight();
    if (!previousHeight) {
        return Result<void>::failure(previousHeight.error());
    }

    auto resized = panel_host_->setPanelHeight(nextHeight);
    if (!resized) {
        return resized;
    }
    auto applied = theme_adapter_->applySnapshot(std::move(snapshot));
    if (!applied) {
        const auto rollback = panel_host_->setPanelHeight(previousHeight.value());
        if (!rollback) {
            reportRecoverable("panel theme rollback", rollback.error());
        }
        return applied;
    }
    auto published = publishThemeProperties();
    if (!published) {
        return published;
    }
    positionLauncher();
    return Result<void>::success();
}

Result<void> ShellRuntime::createViews() {
    if (!theme_adapter_ || !theme_adapter_->current() || !launcher_controller_) {
        return Result<void>::failure(missingPresentationError());
    }

    panel_view_ = std::make_unique<QQuickView>();
    panel_view_->setTitle(tr("Prismdrake Panel"));
    panel_view_->setColor(Qt::transparent);
    panel_view_->setResizeMode(QQuickView::SizeRootObjectToView);
    panel_view_->setInitialProperties(
        {{QStringLiteral("themeGeneration"), objectVariant(theme_adapter_->current())},
         {QStringLiteral("taskModel"), objectVariant(&task_model_)}});
    panel_view_->setSource(
        QUrl(QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/panel/PanelSurface.qml")));
    if (panel_view_->status() == QQuickView::Error || panel_view_->rootObject() == nullptr) {
        return Result<void>::failure(surfaceError());
    }

    launcher_view_ = std::make_unique<QQuickView>();
    launcher_view_->setTitle(tr("Prismdrake Launcher"));
    launcher_view_->setFlags(Qt::Tool | Qt::FramelessWindowHint);
    launcher_view_->setColor(Qt::transparent);
    launcher_view_->setResizeMode(QQuickView::SizeViewToRootObject);
    launcher_view_->setInitialProperties(
        {{QStringLiteral("themeGeneration"), objectVariant(theme_adapter_->current())},
         {QStringLiteral("launcherModel"),
          objectVariant(launcher_controller_->presentationModel())}});
    launcher_view_->setSource(
        QUrl(QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/launcher/LauncherSurface.qml")));
    if (launcher_view_->status() == QQuickView::Error || launcher_view_->rootObject() == nullptr) {
        return Result<void>::failure(surfaceError());
    }
    launcher_view_->hide();

    auto *panel = panel_view_->rootObject();
    auto *launcher = launcher_view_->rootObject();
    const bool connected =
        connect(panel, SIGNAL(launcherRequested()), this, SLOT(openLauncher())) &&
        connect(panel, SIGNAL(focusExitForward()), this, SLOT(leavePanelKeyboardNavigation())) &&
        connect(panel, SIGNAL(focusExitBackward()), this, SLOT(leavePanelKeyboardNavigation())) &&
        connect(launcher, SIGNAL(searchRequested(QString)), this,
                SLOT(handleLauncherSearch(QString))) &&
        connect(launcher, SIGNAL(dismissRequested()), this, SLOT(dismissLauncherToPanel())) &&
        connect(launcher, SIGNAL(focusExitForward()), this, SLOT(dismissLauncherToPanel())) &&
        connect(launcher, SIGNAL(focusExitBackward()), this, SLOT(dismissLauncherToPanel()));
    if (!connected) {
        return Result<void>::failure(connectionError());
    }
    connect(launcher_view_.get(), &QWindow::activeChanged, this, [this]() {
        if (!launcher_view_ || !launcher_view_->isVisible()) {
            return;
        }
        if (launcher_view_->isActive()) {
            launcher_was_active_ = true;
        } else if (launcher_was_active_) {
            dismissLauncherWithoutPanelFocus();
        }
    });
    return Result<void>::success();
}

Result<void> ShellRuntime::publishThemeProperties() {
    if (!theme_adapter_ || !theme_adapter_->current() || !panel_view_ || !launcher_view_ ||
        !panel_view_->rootObject() || !launcher_view_->rootObject()) {
        return Result<void>::failure(missingPresentationError());
    }
    const auto value = objectVariant(theme_adapter_->current());
    if (!panel_view_->rootObject()->setProperty("themeGeneration", value) ||
        !launcher_view_->rootObject()->setProperty("themeGeneration", value)) {
        return Result<void>::failure(surfaceError());
    }
    return Result<void>::success();
}

Result<std::uint32_t> ShellRuntime::projectedPanelHeight() const {
    if (!theme_adapter_ || !theme_adapter_->current()) {
        return Result<std::uint32_t>::failure(missingPresentationError());
    }
    const auto height = theme_adapter_->current()->panel()->panelHeight();
    if (!std::isfinite(height) || height <= 0.0 ||
        height > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return Result<std::uint32_t>::failure(invalidPanelHeight());
    }
    return Result<std::uint32_t>::success(static_cast<std::uint32_t>(std::ceil(height)));
}

void ShellRuntime::positionLauncher() {
    if (!launcher_view_ || !panel_host_ || !panel_host_->placement()) {
        return;
    }
    const auto &placement = *panel_host_->placement();
    const auto &output = placement.output;
    const auto &panel = placement.dock.panel;
    const auto width = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(1, launcher_view_->width())), output.widthPx());
    const auto availableHeight = static_cast<std::uint32_t>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(panel.y) - output.yPx()));
    const auto height = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(1, launcher_view_->height())), availableHeight);
    launcher_view_->setGeometry(static_cast<int>(output.xPx()),
                                static_cast<int>(panel.y - static_cast<std::int32_t>(height)),
                                static_cast<int>(width), static_cast<int>(height));
}

void ShellRuntime::destroyPresentationEpoch() noexcept {
    panel_host_.reset();
    if (launcher_view_) {
        launcher_view_->hide();
    }
    launcher_view_.reset();
    panel_view_.reset();
    theme_adapter_.reset();
    launcher_was_active_ = false;
}

void ShellRuntime::openLauncher() {
    auto actions = state_.openLauncher();
    auto applied = executeActions(actions);
    if (!applied) {
        reportRecoverable("launcher focus entry", applied.error());
    }
}

void ShellRuntime::dismissLauncherToPanel() {
    auto actions = state_.dismissLauncher(true);
    auto applied = executeActions(actions);
    if (!applied) {
        reportRecoverable("launcher focus return", applied.error());
    }
}

void ShellRuntime::dismissLauncherWithoutPanelFocus() {
    auto actions = state_.dismissLauncher(false);
    auto applied = executeActions(actions);
    if (!applied) {
        reportRecoverable("launcher dismissal", applied.error());
    }
}

void ShellRuntime::handleLauncherSearch(const QString &query) {
    auto searched = launcher_controller_->setSearchQuery(query);
    if (!searched) {
        reportRecoverable("launcher query", searched.error());
    }
}

void ShellRuntime::leavePanelKeyboardNavigation() {
    auto actions = state_.leavePanelKeyboardNavigation();
    auto applied = executeActions(actions);
    if (!applied) {
        reportRecoverable("panel focus exit", applied.error());
    }
}

void ShellRuntime::handleX11Loss(const Error &error) {
    const auto actions = state_.loseX11Connection();
    static_cast<void>(executeActions(actions));
    requestShutdown("X11 connection", error);
}

void ShellRuntime::reportRecoverable(const char *context, const Error &error) {
    const auto diagnostic =
        QString::fromLatin1(context) + QLatin1Char('\n') + QString::fromStdString(error.message);
    if (diagnostic == last_recoverable_diagnostic_) {
        return;
    }
    last_recoverable_diagnostic_ = diagnostic;
    qWarning().noquote() << "prismdrake-shell:" << context << '-' << error.message
                         << error.recovery;
}

void ShellRuntime::requestShutdown(const char *context, const Error &error) {
    if (shutdown_requested_) {
        return;
    }
    shutdown_requested_ = true;
    qCritical().noquote() << "prismdrake-shell:" << context << '-' << error.message
                          << error.recovery;
    QCoreApplication::exit(foundation::processExitCode(foundation::exitStatusFor(error)));
}

} // namespace prismdrake::shell::runtime
