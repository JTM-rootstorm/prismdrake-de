#pragma once

#include "DesktopSurfaceController.hpp"
#include "Result.hpp"
#include "X11PropertyWorker.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class QQuickView;
class QScreen;
class QWindow;
class QObject;

namespace prismdrake::shell::desktop {

/// Qt/X11 host for the complete set of per-output desktop surfaces.
///
/// Windows are staged hidden, receive their checked EWMH desktop role before
/// mapping, and never request focus or stacking. The WM remains authoritative.
class DesktopWindowHost final {
  public:
    using ScreenResolver = std::function<QScreen *(std::string_view)>;
    using PublicationCallback =
        std::function<void(const DesktopSurfaceUpdate &, const foundation::Result<void> &)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<DesktopWindowHost>>
    create(std::string_view display, ScreenResolver screenResolver,
           PublicationCallback publicationCallback = {},
           window::X11PropertyWorker::Transaction testTransaction = {});

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
    struct PendingApply;

    DesktopWindowHost(ScreenResolver screenResolver,
                      PublicationCallback publicationCallback) noexcept;

    [[nodiscard]] foundation::Result<std::unique_ptr<WindowEntry>>
    createWindow(QScreen &screen, const DesktopSurfaceState &surface,
                 const std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> &wallpaper);
    [[nodiscard]] foundation::Result<void> beginApply(const DesktopSurfaceUpdate &update);
    void completeApply(std::uint64_t token, foundation::Result<void> result);
    [[nodiscard]] foundation::Result<std::uint64_t> allocateToken();

    ScreenResolver screen_resolver_;
    PublicationCallback publication_callback_;
    std::unique_ptr<QObject> completion_context_;
    std::unique_ptr<window::X11PropertyWorker> property_worker_;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> windows_;
    std::unique_ptr<PendingApply> pending_apply_;
    std::optional<DesktopSurfaceUpdate> queued_update_;
    std::uint64_t next_token_{1U};
    std::shared_ptr<const DesktopSurfaceSnapshot> current_;
};

} // namespace prismdrake::shell::desktop
