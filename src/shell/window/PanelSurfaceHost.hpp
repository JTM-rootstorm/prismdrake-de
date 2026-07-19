#pragma once

#include "AtomCache.hpp"
#include "OutputSurfaceRegistry.hpp"
#include "Result.hpp"
#include "X11Connection.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

class QQuickView;
class QScreen;

namespace prismdrake::shell::window {

/// Atomic per-output panel window host. It consumes only complete registry snapshots and
/// publishes the registry's matching EWMH reservation before any staged panel becomes visible.
class PanelSurfaceHost final {
  public:
    using ScreenResolver = std::function<QScreen *(std::string_view)>;
    using ViewFactory = std::function<foundation::Result<std::unique_ptr<QQuickView>>(
        std::string_view outputName, QScreen &screen)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<PanelSurfaceHost>>
    create(std::string_view display, ScreenResolver screenResolver, ViewFactory viewFactory);
    ~PanelSurfaceHost();

    PanelSurfaceHost(const PanelSurfaceHost &) = delete;
    PanelSurfaceHost &operator=(const PanelSurfaceHost &) = delete;

    [[nodiscard]] foundation::Result<void> apply(const outputs::SurfaceRegistrySnapshot &snapshot);
    [[nodiscard]] foundation::Result<void> requestKeyboardAccess(std::string_view outputName);
    [[nodiscard]] foundation::Result<void> releaseKeyboardAccess();
    [[nodiscard]] QQuickView *view(std::string_view outputName) const noexcept;

  private:
    struct WindowEntry;

    PanelSurfaceHost(x11::X11Connection connection, x11::AtomCache atoms,
                     ScreenResolver screenResolver, ViewFactory viewFactory) noexcept;
    [[nodiscard]] foundation::Result<std::unique_ptr<WindowEntry>>
    createWindow(const outputs::SurfacePlacement &placement, QScreen &screen);
    [[nodiscard]] foundation::Result<void> republish(WindowEntry &entry, bool acceptsFocus);
    void removeProperties(WindowEntry &entry) noexcept;

    x11::X11Connection connection_;
    x11::AtomCache atoms_;
    ScreenResolver screen_resolver_;
    ViewFactory view_factory_;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> windows_;
    std::uint64_t topology_generation_{0U};
};

} // namespace prismdrake::shell::window
