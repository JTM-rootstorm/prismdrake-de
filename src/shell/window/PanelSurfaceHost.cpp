#include "PanelSurfaceHost.hpp"

#include "DockProperties.hpp"

#include <QQuickView>
#include <QScreen>

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

Result<std::unique_ptr<PanelSurfaceHost>> PanelSurfaceHost::create(std::string_view display,
                                                                   ScreenResolver screenResolver,
                                                                   ViewFactory viewFactory) {
    if (!screenResolver || !viewFactory) {
        return invalidHost<std::unique_ptr<PanelSurfaceHost>>();
    }
    auto connection = x11::X11Connection::connect(display);
    if (!connection) {
        return Result<std::unique_ptr<PanelSurfaceHost>>::failure(connection.error());
    }
    auto atoms = x11::AtomCache::create(connection.value());
    if (!atoms) {
        return Result<std::unique_ptr<PanelSurfaceHost>>::failure(atoms.error());
    }
    return Result<std::unique_ptr<PanelSurfaceHost>>::success(std::unique_ptr<PanelSurfaceHost>(
        new PanelSurfaceHost(std::move(connection).value(), std::move(atoms).value(),
                             std::move(screenResolver), std::move(viewFactory))));
}

PanelSurfaceHost::PanelSurfaceHost(x11::X11Connection connection, x11::AtomCache atoms,
                                   ScreenResolver screenResolver, ViewFactory viewFactory) noexcept
    : connection_(std::move(connection)), atoms_(std::move(atoms)),
      screen_resolver_(std::move(screenResolver)), view_factory_(std::move(viewFactory)) {}

PanelSurfaceHost::~PanelSurfaceHost() {
    for (auto &[name, entry] : windows_) {
        (void)name;
        removeProperties(*entry);
        entry->view->hide();
    }
}

Result<std::unique_ptr<PanelSurfaceHost::WindowEntry>>
PanelSurfaceHost::createWindow(const outputs::SurfacePlacement &placement, QScreen &screen) {
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
    view->setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    const auto targetGeometry = geometry(placement);
    view->setGeometry(targetGeometry);
    auto id = windowId(*view);
    if (!id) {
        return Result<std::unique_ptr<WindowEntry>>::failure(id.error());
    }
    auto published = x11::DockProperties::publishBottomPanelReservation(
        connection_, atoms_, id.value(), *placement.panelStrut);
    if (!published) {
        return Result<std::unique_ptr<WindowEntry>>::failure(published.error());
    }
    return Result<std::unique_ptr<WindowEntry>>::success(std::make_unique<WindowEntry>(
        WindowEntry{std::move(view), id.value(), *placement.panelStrut, targetGeometry, &screen}));
}

void PanelSurfaceHost::removeProperties(WindowEntry &entry) noexcept {
    if (connection_.healthy()) {
        (void)x11::DockProperties::remove(connection_, atoms_, entry.publishedWindow);
    }
}

Result<void> PanelSurfaceHost::apply(const outputs::SurfaceRegistrySnapshot &snapshot) {
    if (!connection_.healthy() || snapshot.surfaceGeneration() <= topology_generation_) {
        return invalidSnapshot<void>();
    }
    std::set<std::string> desired;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> staged;
    for (const auto &placement : snapshot.panels()) {
        if (!desired.insert(placement.outputName).second || !placement.panelStrut) {
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
            for (auto &[name, pending] : staged) {
                (void)name;
                removeProperties(*pending);
            }
            return Result<void>::failure(entry.error());
        }
        staged.emplace(placement.outputName, std::move(entry).value());
    }

    for (auto iterator = windows_.begin(); iterator != windows_.end();) {
        if (desired.contains(iterator->first) && !staged.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        removeProperties(*iterator->second);
        iterator->second->view->hide();
        iterator = windows_.erase(iterator);
    }
    while (!staged.empty()) {
        auto node = staged.extract(staged.begin());
        node.mapped()->view->show();
        windows_.insert(std::move(node));
    }
    for (auto &[name, entry] : windows_) {
        (void)name;
        if (!entry->view->isVisible()) {
            entry->view->show();
        }
    }
    topology_generation_ = snapshot.surfaceGeneration();
    return Result<void>::success();
}

Result<void> PanelSurfaceHost::republish(WindowEntry &entry, bool acceptsFocus) {
    removeProperties(entry);
    entry.view->setFlag(Qt::WindowDoesNotAcceptFocus, !acceptsFocus);
    auto id = windowId(*entry.view);
    if (!id) {
        return Result<void>::failure(id.error());
    }
    auto published = x11::DockProperties::publishBottomPanelReservation(
        connection_, atoms_, id.value(), entry.reservation);
    if (!published) {
        return published;
    }
    entry.publishedWindow = id.value();
    return Result<void>::success();
}

Result<void> PanelSurfaceHost::requestKeyboardAccess(std::string_view outputName) {
    const auto found = windows_.find(outputName);
    if (found == windows_.end()) {
        return invalidHost<void>();
    }
    auto published = republish(*found->second, true);
    if (!published) {
        return published;
    }
    found->second->view->requestActivate();
    return Result<void>::success();
}

Result<void> PanelSurfaceHost::releaseKeyboardAccess() {
    for (auto &[name, entry] : windows_) {
        (void)name;
        if (!(entry->view->flags() & Qt::WindowDoesNotAcceptFocus)) {
            auto published = republish(*entry, false);
            if (!published) {
                return published;
            }
        }
    }
    return Result<void>::success();
}

QQuickView *PanelSurfaceHost::view(std::string_view outputName) const noexcept {
    const auto found = windows_.find(outputName);
    return found == windows_.end() ? nullptr : found->second->view.get();
}

} // namespace prismdrake::shell::window
