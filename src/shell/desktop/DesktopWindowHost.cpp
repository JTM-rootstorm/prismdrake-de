#include "DesktopWindowHost.hpp"

#include "DesktopWallpaperItem.hpp"

#include <QGuiApplication>
#include <QObject>
#include <QQuickView>
#include <QScreen>
#include <QUrl>
#include <QWindow>
#include <QtQml/qqml.h>

#include <limits>
#include <set>
#include <utility>

namespace prismdrake::shell::desktop {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidHost() {
    return Result<Value>::failure(
        {ErrorCode::invalid_environment, "The desktop window host is unavailable.",
         "Run Prismdrake with Qt's XCB platform and complete output reconciliation."});
}

template <typename Value> [[nodiscard]] Result<Value> invalidPublication() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The desktop window publication is incomplete.",
         "Apply one complete immutable controller update with a matched Qt screen per output."});
}

[[nodiscard]] Result<x11::WindowId> windowId(QWindow &window) {
    const auto native = window.winId();
    if (native == 0U || native > std::numeric_limits<x11::WindowId::Value>::max()) {
        return invalidHost<x11::WindowId>();
    }
    return x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(native));
}

[[nodiscard]] Result<std::uint64_t> exhaustedToken() {
    return Result<std::uint64_t>::failure(
        {ErrorCode::too_large, "The desktop X11 publication token space is exhausted.",
         "Restart the shell before publishing another desktop generation."});
}

} // namespace

struct DesktopWindowHost::WindowEntry final {
    std::unique_ptr<QQuickView> view;
    DesktopWallpaperItem *wallpaperItem;
    x11::WindowId publishedWindow;
    double devicePixelRatio;
};

struct DesktopWindowHost::PendingApply final {
    PendingApply(std::uint64_t tokenValue, DesktopSurfaceUpdate updateValue)
        : token(tokenValue), update(std::move(updateValue)) {}
    std::uint64_t token;
    DesktopSurfaceUpdate update;
    std::set<std::string> desired;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> staged;
};

Result<std::unique_ptr<DesktopWindowHost>>
DesktopWindowHost::create(std::string_view display, ScreenResolver screenResolver,
                          PublicationCallback publicationCallback,
                          window::X11PropertyWorker::Transaction testTransaction) {
    if (QGuiApplication::instance() == nullptr || !screenResolver) {
        return invalidHost<std::unique_ptr<DesktopWindowHost>>();
    }
    static const int wallpaperType = qmlRegisterType<DesktopWallpaperItem>(
        "org.prismdrake.shell.desktop", 0, 1, "DesktopWallpaper");
    if (wallpaperType < 0) {
        return invalidHost<std::unique_ptr<DesktopWindowHost>>();
    }
    auto host = std::unique_ptr<DesktopWindowHost>(
        new DesktopWindowHost(std::move(screenResolver), std::move(publicationCallback)));
    host->completion_context_ = std::make_unique<QObject>();
    auto worker = window::X11PropertyWorker::create(
        *host->completion_context_, std::string{display},
        [instance = host.get()](std::uint64_t token, Result<void> result) {
            instance->completeApply(token, std::move(result));
        },
        std::move(testTransaction));
    if (!worker) {
        return Result<std::unique_ptr<DesktopWindowHost>>::failure(worker.error());
    }
    host->property_worker_ = std::move(worker).value();
    return Result<std::unique_ptr<DesktopWindowHost>>::success(std::move(host));
}

DesktopWindowHost::DesktopWindowHost(ScreenResolver screenResolver,
                                     PublicationCallback publicationCallback) noexcept
    : screen_resolver_(std::move(screenResolver)),
      publication_callback_(std::move(publicationCallback)) {}

DesktopWindowHost::~DesktopWindowHost() {
    property_worker_.reset();
    for (auto &[name, entry] : windows_) {
        (void)name;
        entry->view->hide();
    }
    windows_.clear();
    pending_apply_.reset();
    completion_context_.reset();
}

Result<std::unique_ptr<DesktopWindowHost::WindowEntry>> DesktopWindowHost::createWindow(
    QScreen &screen, const DesktopSurfaceState &surface,
    const std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> &wallpaper) {
    auto view = std::make_unique<QQuickView>();
    view->setScreen(&screen);
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->setColor(wallpaper->backgroundColor);
    view->setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                   Qt::WindowTransparentForInput);
    view->setGeometry(surface.logicalGeometry);
    view->setSource(
        QUrl{QStringLiteral("qrc:/qt/qml/org/prismdrake/shell/desktop/DesktopSurface.qml")});
    if (view->status() != QQuickView::Ready) {
        return invalidHost<std::unique_ptr<WindowEntry>>();
    }
    auto *wallpaperItem = qobject_cast<DesktopWallpaperItem *>(view->rootObject());
    if (wallpaperItem == nullptr) {
        return invalidHost<std::unique_ptr<WindowEntry>>();
    }
    wallpaperItem->setSurface(wallpaper, surface.wallpaperLayout, surface.devicePixelRatio);
    auto id = windowId(*view);
    if (!id) {
        return Result<std::unique_ptr<WindowEntry>>::failure(id.error());
    }
    return Result<std::unique_ptr<WindowEntry>>::success(std::make_unique<WindowEntry>(
        WindowEntry{std::move(view), wallpaperItem, id.value(), surface.devicePixelRatio}));
}

Result<void> DesktopWindowHost::apply(const DesktopSurfaceUpdate &update) {
    if (!update.snapshot()) {
        return invalidPublication<void>();
    }
    const auto pendingTopology =
        pending_apply_ ? pending_apply_->update.snapshot()->topologyGeneration() : 0U;
    const auto queuedTopology =
        queued_update_ ? queued_update_->snapshot()->topologyGeneration() : 0U;
    const auto currentTopology = current_ ? current_->topologyGeneration() : 0U;
    if (update.snapshot()->topologyGeneration() <
        std::max({pendingTopology, queuedTopology, currentTopology})) {
        return invalidPublication<void>();
    }
    if (pending_apply_) {
        queued_update_ = update;
        return Result<void>::success();
    }
    return beginApply(update);
}

Result<void> DesktopWindowHost::beginApply(const DesktopSurfaceUpdate &update) {
    auto allocatedToken = allocateToken();
    if (!allocatedToken) {
        return Result<void>::failure(allocatedToken.error());
    }
    auto pending = std::make_unique<PendingApply>(allocatedToken.value(), update);
    const auto &snapshot = update.snapshot();
    std::vector<window::X11PropertyOperation> operations;
    for (const auto &surface : snapshot->surfaces()) {
        if (!pending->desired.insert(surface.outputName).second) {
            return invalidPublication<void>();
        }
        auto *screen = screen_resolver_(surface.outputName);
        if (screen == nullptr) {
            return invalidPublication<void>();
        }
        const auto existing = windows_.find(surface.outputName);
        const bool replace = existing == windows_.end() ||
                             existing->second->view->screen() != screen ||
                             existing->second->view->geometry() != surface.logicalGeometry ||
                             existing->second->devicePixelRatio != surface.devicePixelRatio;
        if (!replace) {
            continue;
        }
        auto created = createWindow(*screen, surface, snapshot->wallpaper());
        if (!created) {
            return Result<void>::failure(created.error());
        }
        operations.push_back({window::X11PropertyOperationKind::publishDesktop,
                              created.value()->publishedWindow, std::nullopt});
        pending->staged.emplace(surface.outputName, std::move(created).value());
    }
    const auto token = pending->token;
    pending_apply_ = std::move(pending);
    if (operations.empty()) {
        QMetaObject::invokeMethod(
            completion_context_.get(),
            [this, token] { completeApply(token, Result<void>::success()); }, Qt::QueuedConnection);
        return Result<void>::success();
    }
    auto submitted = property_worker_->submit(token, std::move(operations));
    if (!submitted) {
        pending_apply_.reset();
    }
    return submitted;
}

void DesktopWindowHost::completeApply(std::uint64_t token, Result<void> result) {
    if (!pending_apply_ || pending_apply_->token != token) {
        return;
    }
    auto pending = std::move(pending_apply_);
    if (!result || queued_update_) {
        if (result) {
            for (const auto &[name, entry] : pending->staged) {
                (void)name;
                submitCleanup(entry->publishedWindow);
            }
        } else {
            queued_update_.reset();
            if (publication_callback_) {
                publication_callback_(pending->update, result);
            }
        }
    } else {
        const auto &snapshot = pending->update.snapshot();
        for (const auto &surface : snapshot->surfaces()) {
            if (pending->staged.contains(surface.outputName)) {
                continue;
            }
            auto &entry = *windows_.at(surface.outputName);
            entry.view->setColor(snapshot->wallpaper()->backgroundColor);
            entry.wallpaperItem->setSurface(snapshot->wallpaper(), surface.wallpaperLayout,
                                            surface.devicePixelRatio);
        }
        for (auto iterator = windows_.begin(); iterator != windows_.end();) {
            if (pending->desired.contains(iterator->first) &&
                !pending->staged.contains(iterator->first)) {
                ++iterator;
                continue;
            }
            submitCleanup(iterator->second->publishedWindow);
            iterator->second->view->hide();
            iterator = windows_.erase(iterator);
        }
        while (!pending->staged.empty()) {
            auto node = pending->staged.extract(pending->staged.begin());
            node.mapped()->view->show();
            windows_.insert(std::move(node));
        }
        current_ = snapshot;
        if (publication_callback_) {
            publication_callback_(pending->update, result);
        }
    }
    if (queued_update_) {
        auto queued = std::move(*queued_update_);
        queued_update_.reset();
        auto started = beginApply(queued);
        if (!started && publication_callback_) {
            publication_callback_(queued, started);
        }
    }
}

void DesktopWindowHost::submitCleanup(x11::WindowId windowId) noexcept {
    auto token = allocateToken();
    if (!token) {
        return;
    }
    const auto submitted = property_worker_->submit(
        token.value(), {{window::X11PropertyOperationKind::removeDesktop, windowId, std::nullopt}});
    (void)submitted;
}

Result<std::uint64_t> DesktopWindowHost::allocateToken() {
    if (next_token_ == std::numeric_limits<std::uint64_t>::max()) {
        return exhaustedToken();
    }
    return Result<std::uint64_t>::success(next_token_++);
}

QWindow *DesktopWindowHost::window(std::string_view outputName) const noexcept {
    const auto found = windows_.find(outputName);
    return found == windows_.end() ? nullptr : found->second->view.get();
}

} // namespace prismdrake::shell::desktop
