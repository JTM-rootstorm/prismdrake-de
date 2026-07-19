#pragma once

#include "AtomCache.hpp"
#include "DesktopSurfaceController.hpp"
#include "Result.hpp"
#include "X11Connection.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

class QQuickView;
class QScreen;
class QWindow;

namespace prismdrake::shell::desktop {

/// Qt/X11 host for the complete set of per-output desktop surfaces.
///
/// Windows are staged hidden, receive their checked EWMH desktop role before
/// mapping, and never request focus or stacking. The WM remains authoritative.
class DesktopWindowHost final {
  public:
    using ScreenResolver = std::function<QScreen *(std::string_view)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<DesktopWindowHost>>
    create(std::string_view display, ScreenResolver screenResolver);

    ~DesktopWindowHost();
    DesktopWindowHost(const DesktopWindowHost &) = delete;
    DesktopWindowHost &operator=(const DesktopWindowHost &) = delete;

    /// Applies one complete controller update. Any staging failure leaves the
    /// previous mapped set and wallpaper publication unchanged.
    [[nodiscard]] foundation::Result<void> apply(const DesktopSurfaceUpdate &update);

    [[nodiscard]] std::size_t windowCount() const noexcept { return windows_.size(); }
    [[nodiscard]] QWindow *window(std::string_view outputName) const noexcept;
    [[nodiscard]] const std::shared_ptr<const DesktopSurfaceSnapshot> &current() const noexcept {
        return current_;
    }

  private:
    struct WindowEntry;

    DesktopWindowHost(x11::X11Connection connection, x11::AtomCache atoms,
                      ScreenResolver screenResolver) noexcept;

    [[nodiscard]] foundation::Result<std::unique_ptr<WindowEntry>>
    createWindow(const DesktopSurfaceState &surface,
                 const std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> &wallpaper);
    [[nodiscard]] foundation::Result<void> republishIfRecreated(WindowEntry &entry,
                                                                const DesktopSurfaceState &surface);
    void removeProperties(WindowEntry &entry) noexcept;

    x11::X11Connection connection_;
    x11::AtomCache atoms_;
    ScreenResolver screen_resolver_;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> windows_;
    std::shared_ptr<const DesktopSurfaceSnapshot> current_;
};

} // namespace prismdrake::shell::desktop
