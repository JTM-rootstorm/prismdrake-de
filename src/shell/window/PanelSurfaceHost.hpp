#pragma once

#include "OutputSurfaceRegistry.hpp"
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
class QObject;

namespace prismdrake::shell::window {

/// Atomic per-output panel window host. It consumes only complete registry snapshots and
/// publishes the registry's matching EWMH reservation before any staged panel becomes visible.
class PanelSurfaceHost final {
  public:
    using ScreenResolver = std::function<QScreen *(std::string_view)>;
    using PublicationCallback = std::function<void(const outputs::SurfaceRegistrySnapshot &,
                                                   const foundation::Result<void> &)>;
    using FocusReadyCallback = std::function<void(QQuickView &)>;
    using ViewFactory = std::function<foundation::Result<std::unique_ptr<QQuickView>>(
        std::string_view outputName, QScreen &screen)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<PanelSurfaceHost>>
    create(std::string_view display, ScreenResolver screenResolver, ViewFactory viewFactory,
           PublicationCallback publicationCallback = {},
           X11PropertyWorker::Transaction testTransaction = {});
    ~PanelSurfaceHost();

    PanelSurfaceHost(const PanelSurfaceHost &) = delete;
    PanelSurfaceHost &operator=(const PanelSurfaceHost &) = delete;

    [[nodiscard]] foundation::Result<void> apply(const outputs::SurfaceRegistrySnapshot &snapshot);
    /// Removes every panel and its reservation while retaining the healthy X11 host for a
    /// later settings-owner epoch. The next complete snapshot may be applied at any generation.
    void clear() noexcept;
    [[nodiscard]] foundation::Result<void>
    requestKeyboardAccess(std::string_view outputName, FocusReadyCallback focusReady = {});
    [[nodiscard]] foundation::Result<void> releaseKeyboardAccess();
    [[nodiscard]] QQuickView *view(std::string_view outputName) const noexcept;
    [[nodiscard]] std::uint64_t currentGeneration() const noexcept { return topology_generation_; }

  private:
    struct WindowEntry;
    struct PendingApply;
    struct PendingFocus;

    PanelSurfaceHost(ScreenResolver screenResolver, ViewFactory viewFactory,
                     PublicationCallback publicationCallback) noexcept;
    [[nodiscard]] foundation::Result<std::unique_ptr<WindowEntry>>
    createWindow(const outputs::SurfacePlacement &placement, QScreen &screen,
                 bool acceptsFocus = false);
    [[nodiscard]] foundation::Result<void> beginFocus(std::string outputName, bool acceptsFocus,
                                                      FocusReadyCallback focusReady = {});
    void completeFocus(std::uint64_t token, foundation::Result<void> result);
    void continueQueuedWork();
    [[nodiscard]] foundation::Result<void>
    beginApply(const outputs::SurfaceRegistrySnapshot &snapshot);
    void completeApply(std::uint64_t token, foundation::Result<void> result);
    void handleCompletion(std::uint64_t token, foundation::Result<void> result);
    [[nodiscard]] foundation::Result<std::uint64_t> allocateToken();

    ScreenResolver screen_resolver_;
    ViewFactory view_factory_;
    PublicationCallback publication_callback_;
    std::unique_ptr<QObject> completion_context_;
    std::unique_ptr<X11PropertyWorker> property_worker_;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> windows_;
    std::unique_ptr<PendingApply> pending_apply_;
    std::unique_ptr<PendingFocus> pending_focus_;
    std::optional<outputs::SurfaceRegistrySnapshot> queued_snapshot_;
    std::optional<outputs::SurfaceRegistrySnapshot> current_snapshot_;
    std::optional<std::string> queued_focus_output_;
    FocusReadyCallback queued_focus_callback_;
    std::uint64_t next_token_{1U};
    std::uint64_t topology_generation_{0U};
    bool clear_requested_{false};
    bool focus_request_queued_{false};
};

} // namespace prismdrake::shell::window
