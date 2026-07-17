#include "PanelWindowHost.hpp"

#include "DockProperties.hpp"

#include <QMetaObject>
#include <QRect>
#include <QSocketNotifier>
#include <QWindow>

#include <limits>
#include <utility>
#include <variant>

namespace prismdrake::shell::window {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error visibleWindowError() {
    return {ErrorCode::invalid_argument, "The panel window is already visible.",
            "Attach the host before showing the panel so dock metadata precedes mapping."};
}

[[nodiscard]] foundation::Error missingExitHandler() {
    return {ErrorCode::invalid_argument, "The X11 connection-loss callback is missing.",
            "Provide a callback that exits the shell through its normal shutdown path."};
}

[[nodiscard]] foundation::Error unavailablePlacement() {
    return {ErrorCode::invalid_environment, "No validated panel placement is available.",
            "Complete one checked output-topology observation before showing the panel."};
}

[[nodiscard]] foundation::Error invalidNativeWindow() {
    return {ErrorCode::invalid_environment, "Qt did not provide a usable X11 panel window.",
            "Run the development panel with the Qt XCB platform on the selected display."};
}

[[nodiscard]] foundation::Error lostConnection() {
    return {ErrorCode::io_error, "The panel's X11 connection disappeared.",
            "Exit the shell normally; the session supervisor may restart it after X recovers."};
}

[[nodiscard]] foundation::Error terminatedHost() {
    return {ErrorCode::cancelled, "The panel window host has already stopped.",
            "Do not issue panel-window requests after X11 connection loss."};
}

[[nodiscard]] bool requestsTopologyRefresh(const x11::RootEvent &event) noexcept {
    return std::holds_alternative<x11::RootGeometryHint>(event) ||
           std::holds_alternative<x11::OutputTopologyRefreshHint>(event) ||
           std::holds_alternative<x11::ProtocolErrorHint>(event);
}

} // namespace

Result<std::unique_ptr<PanelWindowHost>>
PanelWindowHost::create(QWindow &window, std::string_view display, std::uint32_t panelHeight,
                        ConnectionLostCallback connectionLost, FailureCallback recoverableFailure) {
    if (window.isVisible()) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(visibleWindowError());
    }
    if (!connectionLost) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(missingExitHandler());
    }

    auto controller = PanelWindowController::create(panelHeight);
    if (!controller) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(controller.error());
    }
    auto connection = x11::X11Connection::connect(display);
    if (!connection) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(connection.error());
    }
    auto atoms = x11::AtomCache::create(connection.value());
    if (!atoms) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(atoms.error());
    }
    auto randr = x11::RandrTopologyProtocol::negotiate(connection.value());
    if (!randr) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(randr.error());
    }
    auto events = x11::RootEventStream::create(connection.value(), randr.value());
    if (!events) {
        return Result<std::unique_ptr<PanelWindowHost>>::failure(events.error());
    }

    auto host = std::unique_ptr<PanelWindowHost>(new PanelWindowHost(
        window, std::move(controller).value(), std::move(connection).value(),
        std::move(atoms).value(), std::move(randr).value(), std::move(events).value(),
        std::move(connectionLost), std::move(recoverableFailure)));

    const auto originalFlags = window.flags();
    window.setFlag(Qt::FramelessWindowHint, true);
    window.setFlag(Qt::WindowDoesNotAcceptFocus, true);
    auto initialized = host->refreshTopology();
    if (!initialized) {
        window.hide();
        window.setFlags(originalFlags);
        return Result<std::unique_ptr<PanelWindowHost>>::failure(initialized.error());
    }

    host->event_notifier_ = std::make_unique<QSocketNotifier>(
        host->connection_.eventFileDescriptor(), QSocketNotifier::Read, &window);
    QObject::connect(host->event_notifier_.get(), &QSocketNotifier::activated, &window,
                     [instance = host.get()](QSocketDescriptor, QSocketNotifier::Type) {
                         instance->drainEvents();
                     });
    return Result<std::unique_ptr<PanelWindowHost>>::success(std::move(host));
}

PanelWindowHost::PanelWindowHost(QWindow &window, PanelWindowController controller,
                                 x11::X11Connection connection, x11::AtomCache atoms,
                                 x11::RandrTopologyProtocol randr, x11::RootEventStream events,
                                 ConnectionLostCallback connectionLost,
                                 FailureCallback recoverableFailure)
    : window_(window), controller_(std::move(controller)), connection_(std::move(connection)),
      atoms_(std::move(atoms)), randr_(std::move(randr)), events_(std::move(events)),
      connection_lost_(std::move(connectionLost)),
      recoverable_failure_(std::move(recoverableFailure)) {}

PanelWindowHost::~PanelWindowHost() {
    if (event_notifier_) {
        event_notifier_->setEnabled(false);
    }
    if (published_window_ && connection_.healthy()) {
        static_cast<void>(x11::DockProperties::remove(connection_, atoms_, *published_window_));
    }
    window_.hide();
}

Result<void> PanelWindowHost::refreshTopology() {
    if (terminated_) {
        return Result<void>::failure(terminatedHost());
    }

    auto wireSnapshot = randr_.query(connection_);
    if (!wireSnapshot) {
        return Result<void>::failure(wireSnapshot.error());
    }
    auto update = controller_.prepare(wireSnapshot.value());
    if (!update) {
        return Result<void>::failure(update.error());
    }
    return applyUpdate(std::move(update).value());
}

Result<x11::WindowId> PanelWindowHost::currentWindowId() {
    const auto native = window_.winId();
    if (native == 0U || native > std::numeric_limits<x11::WindowId::Value>::max()) {
        return Result<x11::WindowId>::failure(invalidNativeWindow());
    }
    return x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(native));
}

void PanelWindowHost::applyGeometry(const PanelWindowPlacement &placement) {
    const auto &panel = placement.dock.panel;
    window_.setGeometry(QRect{static_cast<int>(panel.x), static_cast<int>(panel.y),
                              static_cast<int>(panel.width), static_cast<int>(panel.height)});
    window_.show();
}

void PanelWindowHost::hideIncoherentWindow() {
    window_.hide();
    published_window_.reset();
    applied_.reset();
}

Result<void> PanelWindowHost::applyUpdate(PanelWindowUpdate update) {
    if (terminated_) {
        return Result<void>::failure(terminatedHost());
    }

    auto windowId = currentWindowId();
    if (!windowId) {
        if (published_window_ && connection_.healthy()) {
            static_cast<void>(x11::DockProperties::remove(connection_, atoms_, *published_window_));
        }
        hideIncoherentWindow();
        return Result<void>::failure(windowId.error());
    }

    auto published = x11::DockProperties::publishBottomPanel(
        connection_, atoms_, windowId.value(), update.placement().root, update.placement().output,
        update.panelHeight());
    if (!published) {
        if (controller_.current() && connection_.healthy()) {
            const auto &previous = *controller_.current();
            auto rolledBack = x11::DockProperties::publishBottomPanel(
                connection_, atoms_, windowId.value(), previous.root, previous.output,
                controller_.panelHeight());
            if (rolledBack) {
                applyGeometry(previous);
                published_window_ = windowId.value();
                applied_ = previous;
            } else {
                hideIncoherentWindow();
                reportRecoverable(rolledBack.error());
            }
        } else {
            hideIncoherentWindow();
        }
        return Result<void>::failure(published.error());
    }

    if (published_window_ && *published_window_ != windowId.value() && connection_.healthy()) {
        auto removed = x11::DockProperties::remove(connection_, atoms_, *published_window_);
        if (!removed) {
            static_cast<void>(x11::DockProperties::remove(connection_, atoms_, windowId.value()));
            hideIncoherentWindow();
            return Result<void>::failure(removed.error());
        }
    }

    applyGeometry(update.placement());
    published_window_ = windowId.value();
    applied_ = update.placement();
    controller_.commit(std::move(update));
    return Result<void>::success();
}

Result<void> PanelWindowHost::reapplyCurrent() {
    auto update = controller_.currentUpdate();
    if (!update) {
        return Result<void>::failure(unavailablePlacement());
    }
    return applyUpdate(std::move(update).value());
}

Result<void> PanelWindowHost::requestKeyboardAccess() {
    if (terminated_) {
        return Result<void>::failure(terminatedHost());
    }
    if (keyboard_access_enabled_) {
        window_.requestActivate();
        return Result<void>::success();
    }

    window_.setFlag(Qt::WindowDoesNotAcceptFocus, false);
    auto applied = reapplyCurrent();
    if (!applied) {
        window_.setFlag(Qt::WindowDoesNotAcceptFocus, true);
        const auto rolledBack = reapplyCurrent();
        if (!rolledBack) {
            reportRecoverable(rolledBack.error());
        }
        return applied;
    }
    keyboard_access_enabled_ = true;
    window_.requestActivate();
    return Result<void>::success();
}

Result<void> PanelWindowHost::releaseKeyboardAccess() {
    if (terminated_) {
        return Result<void>::failure(terminatedHost());
    }
    if (!keyboard_access_enabled_) {
        return Result<void>::success();
    }

    window_.setFlag(Qt::WindowDoesNotAcceptFocus, true);
    auto applied = reapplyCurrent();
    if (!applied) {
        window_.setFlag(Qt::WindowDoesNotAcceptFocus, false);
        const auto rolledBack = reapplyCurrent();
        if (!rolledBack) {
            reportRecoverable(rolledBack.error());
        }
        return applied;
    }
    keyboard_access_enabled_ = false;
    return Result<void>::success();
}

Result<void> PanelWindowHost::setPanelHeight(std::uint32_t panelHeight) {
    if (terminated_) {
        return Result<void>::failure(terminatedHost());
    }

    auto wireSnapshot = randr_.query(connection_);
    if (!wireSnapshot) {
        return Result<void>::failure(wireSnapshot.error());
    }
    auto update = controller_.prepare(wireSnapshot.value(), panelHeight);
    if (!update) {
        return Result<void>::failure(update.error());
    }
    return applyUpdate(std::move(update).value());
}

void PanelWindowHost::drainEvents() {
    if (terminated_) {
        return;
    }

    auto batch = events_.drain();
    if (!batch) {
        if (!connection_.healthy()) {
            terminateForConnectionLoss(batch.error());
        } else {
            reportRecoverable(batch.error());
        }
        return;
    }
    if (!connection_.healthy()) {
        terminateForConnectionLoss(lostConnection());
        return;
    }

    bool refresh = false;
    for (const auto &event : batch.value().events) {
        refresh = refresh || requestsTopologyRefresh(event);
    }
    if (!refresh) {
        return;
    }

    auto refreshed = refreshTopology();
    if (!refreshed) {
        if (!connection_.healthy()) {
            terminateForConnectionLoss(refreshed.error());
        } else {
            reportRecoverable(refreshed.error());
        }
    }
}

void PanelWindowHost::reportRecoverable(const foundation::Error &error) const {
    if (recoverable_failure_) {
        recoverable_failure_(error);
    }
}

void PanelWindowHost::terminateForConnectionLoss(const foundation::Error &error) {
    if (terminated_) {
        return;
    }
    terminated_ = true;
    if (event_notifier_) {
        event_notifier_->setEnabled(false);
    }
    hideIncoherentWindow();

    auto callback = connection_lost_;
    QMetaObject::invokeMethod(
        &window_, [callback = std::move(callback), error]() { callback(error); },
        Qt::QueuedConnection);
}

} // namespace prismdrake::shell::window
