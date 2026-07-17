#pragma once

#include "AtomCache.hpp"
#include "PanelWindowController.hpp"
#include "RandrTopology.hpp"
#include "Result.hpp"
#include "RootEventStream.hpp"
#include "X11Connection.hpp"
#include "X11Types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

class QSocketNotifier;
class QWindow;

namespace prismdrake::shell::window {

/// Qt/X11 development host for the single fixed-visible PD1 panel surface.
///
/// The caller owns the QWindow, which must be initially hidden and outlive this host. The host
/// publishes only standards-based dock metadata, never takes WM authority, and does not depend on
/// settings or QML. Its connection-loss callback is queued after the window has been hidden and
/// must arrange normal process shutdown.
class PanelWindowHost final {
  public:
    using FailureCallback = std::function<void(const foundation::Error &)>;
    using ConnectionLostCallback = std::function<void(const foundation::Error &)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<PanelWindowHost>>
    create(QWindow &window, std::string_view display, std::uint32_t panelHeight,
           ConnectionLostCallback connectionLost, FailureCallback recoverableFailure = {});

    ~PanelWindowHost();
    PanelWindowHost(const PanelWindowHost &) = delete;
    PanelWindowHost &operator=(const PanelWindowHost &) = delete;

    /// Makes the panel focusable only after an explicit shell-access action and asks the WM to
    /// activate it. Ordinary startup never makes this request.
    [[nodiscard]] foundation::Result<void> requestKeyboardAccess();

    /// Restores the startup no-focus behavior after keyboard interaction leaves the panel.
    [[nodiscard]] foundation::Result<void> releaseKeyboardAccess();

    /// Applies a runtime panel-height change against one fresh complete topology observation.
    /// Validation or X11 publication failure retains the previously applied height and placement.
    [[nodiscard]] foundation::Result<void> setPanelHeight(std::uint32_t panelHeight);

    [[nodiscard]] const std::optional<PanelWindowPlacement> &placement() const noexcept {
        return applied_;
    }
    [[nodiscard]] bool keyboardAccessEnabled() const noexcept { return keyboard_access_enabled_; }
    [[nodiscard]] bool terminated() const noexcept { return terminated_; }

  private:
    PanelWindowHost(QWindow &window, PanelWindowController controller,
                    x11::X11Connection connection, x11::AtomCache atoms,
                    x11::RandrTopologyProtocol randr, x11::RootEventStream events,
                    ConnectionLostCallback connectionLost, FailureCallback recoverableFailure);

    [[nodiscard]] foundation::Result<void> refreshTopology();
    [[nodiscard]] foundation::Result<void> applyUpdate(PanelWindowUpdate update);
    [[nodiscard]] foundation::Result<void> reapplyCurrent();
    [[nodiscard]] foundation::Result<x11::WindowId> currentWindowId();
    void applyGeometry(const PanelWindowPlacement &placement);
    void hideIncoherentWindow();
    void drainEvents();
    void reportRecoverable(const foundation::Error &error) const;
    void terminateForConnectionLoss(const foundation::Error &error);

    QWindow &window_;
    PanelWindowController controller_;
    x11::X11Connection connection_;
    x11::AtomCache atoms_;
    x11::RandrTopologyProtocol randr_;
    x11::RootEventStream events_;
    ConnectionLostCallback connection_lost_;
    FailureCallback recoverable_failure_;
    std::unique_ptr<QSocketNotifier> event_notifier_;
    std::optional<x11::WindowId> published_window_;
    std::optional<PanelWindowPlacement> applied_;
    bool keyboard_access_enabled_{false};
    bool terminated_{false};
};

} // namespace prismdrake::shell::window
