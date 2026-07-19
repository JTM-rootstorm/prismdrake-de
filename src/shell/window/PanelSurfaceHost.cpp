#include "PanelSurfaceHost.hpp"

#include <QObject>
#include <QQuickView>
#include <QScreen>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace prismdrake::shell::window {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidHost() {
    return Result<Value>::failure(
        {ErrorCode::invalid_environment, "The per-output panel host is unavailable.",
         "Run Prismdrake on Qt XCB with one matched QScreen per configured panel output."});
}

template <typename Value> [[nodiscard]] Result<Value> invalidSnapshot() {
    return Result<Value>::failure({ErrorCode::validation_error,
                                   "The panel surface snapshot is incomplete or stale.",
                                   "Apply one newer complete output-registry generation."});
}

[[nodiscard]] Result<std::uint64_t> exhaustedToken() {
    return Result<std::uint64_t>::failure(
        {ErrorCode::too_large, "The panel X11 publication token space is exhausted.",
         "Restart the shell before publishing another panel generation."});
}

[[nodiscard]] Result<x11::WindowId> windowId(QQuickView &view) {
    const auto native = view.winId();
    if (native == 0U || native > std::numeric_limits<x11::WindowId::Value>::max()) {
        return invalidHost<x11::WindowId>();
    }
    return x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(native));
}

[[nodiscard]] QRect geometry(const outputs::SurfacePlacement &placement) {
    return {placement.logicalGeometry.x, placement.logicalGeometry.y,
            static_cast<int>(placement.logicalGeometry.width),
            static_cast<int>(placement.logicalGeometry.height)};
}

} // namespace

struct PanelSurfaceHost::WindowEntry final {
    std::unique_ptr<QQuickView> view;
    x11::WindowId publishedWindow;
    x11::BottomPanelStrut reservation;
    QRect geometry;
    QScreen *screen;
};

struct PanelSurfaceHost::PendingApply final {
    std::uint64_t token;
    outputs::SurfaceRegistrySnapshot snapshot;
    std::set<std::string> desired;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> staged;
};

struct PanelSurfaceHost::PendingFocus final {
    std::uint64_t token;
    std::string outputName;
    outputs::SurfaceRegistrySnapshot snapshot;
    x11::WindowId oldWindow;
    bool acceptsFocus;
    std::unique_ptr<WindowEntry> replacement;
};

Result<std::unique_ptr<PanelSurfaceHost>>
PanelSurfaceHost::create(std::string_view display, ScreenResolver screenResolver,
                         ViewFactory viewFactory, PublicationCallback publicationCallback,
                         X11PropertyWorker::Transaction testTransaction) {
    if (!screenResolver || !viewFactory) {
        return invalidHost<std::unique_ptr<PanelSurfaceHost>>();
    }
    auto host = std::unique_ptr<PanelSurfaceHost>(new PanelSurfaceHost(
        std::move(screenResolver), std::move(viewFactory), std::move(publicationCallback)));
    host->completion_context_ = std::make_unique<QObject>();
    auto worker = X11PropertyWorker::create(
        *host->completion_context_, std::string{display},
        [instance = host.get()](std::uint64_t token, Result<void> result) {
            instance->handleCompletion(token, std::move(result));
        },
        std::move(testTransaction));
    if (!worker) {
        return Result<std::unique_ptr<PanelSurfaceHost>>::failure(worker.error());
    }
    host->property_worker_ = std::move(worker).value();
    return Result<std::unique_ptr<PanelSurfaceHost>>::success(std::move(host));
}

PanelSurfaceHost::PanelSurfaceHost(ScreenResolver screenResolver, ViewFactory viewFactory,
                                   PublicationCallback publicationCallback) noexcept
    : screen_resolver_(std::move(screenResolver)), view_factory_(std::move(viewFactory)),
      publication_callback_(std::move(publicationCallback)) {}

PanelSurfaceHost::~PanelSurfaceHost() {
    property_worker_.reset();
    for (auto &[name, entry] : windows_) {
        (void)name;
        entry->view->hide();
    }
    windows_.clear();
    pending_apply_.reset();
    completion_context_.reset();
}

Result<std::unique_ptr<PanelSurfaceHost::WindowEntry>>
PanelSurfaceHost::createWindow(const outputs::SurfacePlacement &placement, QScreen &screen,
                               bool acceptsFocus) {
    if (!placement.panelStrut) {
        return invalidSnapshot<std::unique_ptr<WindowEntry>>();
    }
    auto created = view_factory_(placement.outputName, screen);
    if (!created || !created.value()) {
        return created ? invalidHost<std::unique_ptr<WindowEntry>>()
                       : Result<std::unique_ptr<WindowEntry>>::failure(created.error());
    }
    auto view = std::move(created).value();
    view->setScreen(&screen);
    auto flags = Qt::Window | Qt::FramelessWindowHint;
    if (!acceptsFocus) {
        flags |= Qt::WindowDoesNotAcceptFocus;
    }
    view->setFlags(flags);
    const auto targetGeometry = geometry(placement);
    view->setGeometry(targetGeometry);
    auto id = windowId(*view);
    if (!id) {
        return Result<std::unique_ptr<WindowEntry>>::failure(id.error());
    }
    return Result<std::unique_ptr<WindowEntry>>::success(std::make_unique<WindowEntry>(
        WindowEntry{std::move(view), id.value(), *placement.panelStrut, targetGeometry, &screen}));
}

Result<void> PanelSurfaceHost::apply(const outputs::SurfaceRegistrySnapshot &snapshot) {
    const auto latestQueued = queued_snapshot_ ? queued_snapshot_->surfaceGeneration() : 0U;
    const auto latestPending = pending_apply_ ? pending_apply_->snapshot.surfaceGeneration() : 0U;
    if (snapshot.surfaceGeneration() <=
        std::max({topology_generation_, latestQueued, latestPending})) {
        return invalidSnapshot<void>();
    }
    clear_requested_ = false;
    if (pending_apply_ || pending_focus_) {
        queued_snapshot_ = snapshot;
        return Result<void>::success();
    }
    return beginApply(snapshot);
}

Result<void> PanelSurfaceHost::beginApply(const outputs::SurfaceRegistrySnapshot &snapshot) {
    auto token = allocateToken();
    if (!token) {
        return Result<void>::failure(token.error());
    }
    auto pending = std::make_unique<PendingApply>();
    pending->token = token.value();
    pending->snapshot = snapshot;
    std::vector<X11PropertyOperation> operations;
    for (const auto &placement : snapshot.panels()) {
        if (!pending->desired.insert(placement.outputName).second || !placement.panelStrut) {
            return invalidSnapshot<void>();
        }
        auto *screen = screen_resolver_(placement.outputName);
        if (screen == nullptr) {
            return invalidHost<void>();
        }
        const auto found = windows_.find(placement.outputName);
        const bool replace = found == windows_.end() || found->second->screen != screen ||
                             found->second->geometry != geometry(placement) ||
                             found->second->reservation != *placement.panelStrut;
        if (!replace) {
            continue;
        }
        auto entry = createWindow(placement, *screen);
        if (!entry) {
            return Result<void>::failure(entry.error());
        }
        operations.push_back({X11PropertyOperationKind::publishDock, entry.value()->publishedWindow,
                              entry.value()->reservation});
        pending->staged.emplace(placement.outputName, std::move(entry).value());
    }
    const auto publicationToken = pending->token;
    pending_apply_ = std::move(pending);
    if (operations.empty()) {
        QMetaObject::invokeMethod(
            completion_context_.get(),
            [this, publicationToken] { completeApply(publicationToken, Result<void>::success()); },
            Qt::QueuedConnection);
        return Result<void>::success();
    }
    auto submitted = property_worker_->submit(publicationToken, std::move(operations));
    if (!submitted) {
        pending_apply_.reset();
        return submitted;
    }
    return Result<void>::success();
}

void PanelSurfaceHost::handleCompletion(std::uint64_t token, Result<void> result) {
    if (pending_apply_ && pending_apply_->token == token) {
        completeApply(token, std::move(result));
        return;
    }
    if (pending_focus_ && pending_focus_->token == token) {
        completeFocus(token, std::move(result));
    }
}

void PanelSurfaceHost::completeApply(std::uint64_t token, Result<void> result) {
    if (!pending_apply_ || pending_apply_->token != token) {
        return;
    }
    auto pending = std::move(pending_apply_);
    if (!result || clear_requested_ || queued_snapshot_) {
        if (result) {
            for (const auto &[name, entry] : pending->staged) {
                (void)name;
                submitCleanup(entry->publishedWindow, X11PropertyOperationKind::removeDock);
            }
        }
        if (!result && !clear_requested_ && publication_callback_) {
            queued_snapshot_.reset();
            publication_callback_(pending->snapshot, result);
        }
    } else {
        for (auto iterator = windows_.begin(); iterator != windows_.end();) {
            if (pending->desired.contains(iterator->first) &&
                !pending->staged.contains(iterator->first)) {
                ++iterator;
                continue;
            }
            submitCleanup(iterator->second->publishedWindow, X11PropertyOperationKind::removeDock);
            iterator->second->view->hide();
            iterator = windows_.erase(iterator);
        }
        while (!pending->staged.empty()) {
            auto node = pending->staged.extract(pending->staged.begin());
            node.mapped()->view->show();
            windows_.insert(std::move(node));
        }
        topology_generation_ = pending->snapshot.surfaceGeneration();
        current_snapshot_ = pending->snapshot;
        if (publication_callback_) {
            publication_callback_(pending->snapshot, result);
        }
    }
    continueQueuedWork();
}

void PanelSurfaceHost::submitCleanup(x11::WindowId window, X11PropertyOperationKind kind) noexcept {
    auto token = allocateToken();
    if (!token) {
        return;
    }
    const auto submitted = property_worker_->submit(token.value(), {{kind, window, std::nullopt}});
    (void)submitted;
}

Result<std::uint64_t> PanelSurfaceHost::allocateToken() {
    if (next_token_ == std::numeric_limits<std::uint64_t>::max()) {
        return exhaustedToken();
    }
    return Result<std::uint64_t>::success(next_token_++);
}

void PanelSurfaceHost::clear() noexcept {
    clear_requested_ = true;
    queued_snapshot_.reset();
    queued_focus_output_.reset();
    focus_request_queued_ = false;
    pending_focus_.reset();
    for (auto &[name, entry] : windows_) {
        (void)name;
        submitCleanup(entry->publishedWindow, X11PropertyOperationKind::removeDock);
        entry->view->hide();
    }
    windows_.clear();
    topology_generation_ = 0U;
    current_snapshot_.reset();
}

Result<void> PanelSurfaceHost::beginFocus(std::string outputName, bool acceptsFocus) {
    if (pending_apply_ || pending_focus_ || !current_snapshot_) {
        return invalidHost<void>();
    }
    const auto found = windows_.find(outputName);
    const auto placement = std::find_if(
        current_snapshot_->panels().begin(), current_snapshot_->panels().end(),
        [&outputName](const auto &candidate) { return candidate.outputName == outputName; });
    if (found == windows_.end() || placement == current_snapshot_->panels().end()) {
        return invalidHost<void>();
    }
    auto replacement = createWindow(*placement, *found->second->screen, acceptsFocus);
    if (!replacement) {
        return Result<void>::failure(replacement.error());
    }
    auto token = allocateToken();
    if (!token) {
        return Result<void>::failure(token.error());
    }
    const auto oldWindow = found->second->publishedWindow;
    const auto newWindow = replacement.value()->publishedWindow;
    std::vector<X11PropertyOperation> operations{
        {X11PropertyOperationKind::publishDock, newWindow, replacement.value()->reservation}};
    pending_focus_ = std::make_unique<PendingFocus>(
        PendingFocus{token.value(), std::move(outputName), *current_snapshot_, oldWindow,
                     acceptsFocus, std::move(replacement).value()});
    auto submitted = property_worker_->submit(token.value(), std::move(operations));
    if (!submitted) {
        pending_focus_.reset();
    }
    return submitted;
}

void PanelSurfaceHost::completeFocus(std::uint64_t token, Result<void> result) {
    if (!pending_focus_ || pending_focus_->token != token) {
        return;
    }
    auto pending = std::move(pending_focus_);
    if (!result) {
        queued_snapshot_.reset();
        queued_focus_output_.reset();
        focus_request_queued_ = false;
        if (!clear_requested_ && publication_callback_) {
            publication_callback_(pending->snapshot, result);
        }
        return;
    }
    const auto current = windows_.find(pending->outputName);
    const bool superseded =
        clear_requested_ || queued_snapshot_ || focus_request_queued_ || !current_snapshot_ ||
        current_snapshot_->surfaceGeneration() != pending->snapshot.surfaceGeneration() ||
        current == windows_.end() || current->second->publishedWindow != pending->oldWindow;
    if (superseded) {
        if (current == windows_.end() ||
            current->second->publishedWindow != pending->replacement->publishedWindow) {
            submitCleanup(pending->replacement->publishedWindow,
                          X11PropertyOperationKind::removeDock);
        }
    } else {
        current->second->view->hide();
        pending->replacement->view->show();
        current->second = std::move(pending->replacement);
        if (current->second->publishedWindow != pending->oldWindow) {
            submitCleanup(pending->oldWindow, X11PropertyOperationKind::removeDock);
        }
        if (pending->acceptsFocus) {
            current->second->view->requestActivate();
        }
    }
    continueQueuedWork();
}

void PanelSurfaceHost::continueQueuedWork() {
    if (clear_requested_ || pending_apply_ || pending_focus_) {
        return;
    }
    if (queued_snapshot_) {
        auto queued = std::move(*queued_snapshot_);
        queued_snapshot_.reset();
        auto started = beginApply(queued);
        if (!started && publication_callback_) {
            publication_callback_(queued, started);
        }
        return;
    }
    if (!focus_request_queued_) {
        return;
    }
    focus_request_queued_ = false;
    auto output = std::move(queued_focus_output_);
    queued_focus_output_.reset();
    if (output) {
        auto started = beginFocus(std::move(*output), true);
        if (!started && publication_callback_ && current_snapshot_) {
            publication_callback_(*current_snapshot_, started);
        }
        return;
    }
    const auto focused = std::find_if(windows_.begin(), windows_.end(), [](const auto &item) {
        return !(item.second->view->flags() & Qt::WindowDoesNotAcceptFocus);
    });
    if (focused != windows_.end()) {
        auto started = beginFocus(focused->first, false);
        if (!started && publication_callback_ && current_snapshot_) {
            publication_callback_(*current_snapshot_, started);
        }
    }
}

Result<void> PanelSurfaceHost::requestKeyboardAccess(std::string_view outputName) {
    if (!windows_.contains(outputName)) {
        return invalidHost<void>();
    }
    if (pending_apply_ || pending_focus_) {
        queued_focus_output_ = std::string{outputName};
        focus_request_queued_ = true;
        return Result<void>::success();
    }
    return beginFocus(std::string{outputName}, true);
}

Result<void> PanelSurfaceHost::releaseKeyboardAccess() {
    if (pending_apply_ || pending_focus_) {
        queued_focus_output_.reset();
        focus_request_queued_ = true;
        return Result<void>::success();
    }
    const auto focused = std::find_if(windows_.begin(), windows_.end(), [](const auto &item) {
        return !(item.second->view->flags() & Qt::WindowDoesNotAcceptFocus);
    });
    return focused == windows_.end() ? Result<void>::success() : beginFocus(focused->first, false);
}

QQuickView *PanelSurfaceHost::view(std::string_view outputName) const noexcept {
    const auto found = windows_.find(outputName);
    return found == windows_.end() ? nullptr : found->second->view.get();
}

} // namespace prismdrake::shell::window
