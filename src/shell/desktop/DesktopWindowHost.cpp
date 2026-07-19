#include "DesktopWindowHost.hpp"

#include "DesktopProperties.hpp"
#include "DesktopWallpaperItem.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
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

} // namespace

struct DesktopWindowHost::WindowEntry final {
    std::unique_ptr<QQuickView> view;
    DesktopWallpaperItem *wallpaperItem;
    x11::WindowId publishedWindow;
    double devicePixelRatio;
};

Result<std::unique_ptr<DesktopWindowHost>>
DesktopWindowHost::create(std::string_view display, ScreenResolver screenResolver) {
    if (QGuiApplication::instance() == nullptr || !screenResolver) {
        return invalidHost<std::unique_ptr<DesktopWindowHost>>();
    }
    static const int wallpaperType = qmlRegisterType<DesktopWallpaperItem>(
        "org.prismdrake.shell.desktop", 0, 1, "DesktopWallpaper");
    if (wallpaperType < 0) {
        return invalidHost<std::unique_ptr<DesktopWindowHost>>();
    }
    auto connection = x11::X11Connection::connect(display);
    if (!connection) {
        return Result<std::unique_ptr<DesktopWindowHost>>::failure(connection.error());
    }
    auto atoms = x11::AtomCache::create(connection.value());
    if (!atoms) {
        return Result<std::unique_ptr<DesktopWindowHost>>::failure(atoms.error());
    }
    return Result<std::unique_ptr<DesktopWindowHost>>::success(
        std::unique_ptr<DesktopWindowHost>(new DesktopWindowHost(
            std::move(connection).value(), std::move(atoms).value(), std::move(screenResolver))));
}

DesktopWindowHost::DesktopWindowHost(x11::X11Connection connection, x11::AtomCache atoms,
                                     ScreenResolver screenResolver) noexcept
    : connection_(std::move(connection)), atoms_(std::move(atoms)),
      screen_resolver_(std::move(screenResolver)) {}

DesktopWindowHost::~DesktopWindowHost() {
    for (auto &[name, entry] : windows_) {
        (void)name;
        removeProperties(*entry);
        entry->view->hide();
    }
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
    auto published = x11::DesktopProperties::publish(connection_, atoms_, id.value());
    if (!published) {
        return Result<std::unique_ptr<WindowEntry>>::failure(published.error());
    }
    return Result<std::unique_ptr<WindowEntry>>::success(std::make_unique<WindowEntry>(
        WindowEntry{std::move(view), wallpaperItem, id.value(), surface.devicePixelRatio}));
}

void DesktopWindowHost::removeProperties(WindowEntry &entry) noexcept {
    if (!connection_.healthy()) {
        return;
    }
    const auto removed = x11::DesktopProperties::remove(connection_, atoms_, entry.publishedWindow);
    (void)removed;
}

Result<void> DesktopWindowHost::apply(const DesktopSurfaceUpdate &update) {
    if (!update.snapshot() || !connection_.healthy()) {
        return invalidPublication<void>();
    }
    const auto &snapshot = update.snapshot();
    std::set<std::string> desiredNames;
    std::map<std::string, std::unique_ptr<WindowEntry>, std::less<>> staged;
    for (const auto &surface : snapshot->surfaces()) {
        if (!desiredNames.insert(surface.outputName).second) {
            return invalidPublication<void>();
        }
        QScreen *screen = screen_resolver_(surface.outputName);
        if (screen == nullptr) {
            for (auto &[name, entry] : staged) {
                (void)name;
                removeProperties(*entry);
            }
            return invalidPublication<void>();
        }
        const auto existing = windows_.find(surface.outputName);
        const bool needsReplacement =
            existing == windows_.end() || existing->second->view->screen() != screen ||
            existing->second->view->geometry() != surface.logicalGeometry ||
            existing->second->devicePixelRatio != surface.devicePixelRatio;
        if (!needsReplacement) {
            continue;
        }
        auto created = createWindow(*screen, surface, snapshot->wallpaper());
        if (!created) {
            for (auto &[name, entry] : staged) {
                (void)name;
                removeProperties(*entry);
            }
            return Result<void>::failure(created.error());
        }
        staged.emplace(surface.outputName, std::move(created).value());
    }

    // From this point onward there are no fallible operations. Existing
    // windows have not been mutated, remapped, or republished during staging.
    for (const auto &surface : snapshot->surfaces()) {
        if (staged.contains(surface.outputName)) {
            continue;
        }
        auto &entry = *windows_.at(surface.outputName);
        entry.view->setColor(snapshot->wallpaper()->backgroundColor);
        entry.wallpaperItem->setSurface(snapshot->wallpaper(), surface.wallpaperLayout,
                                        surface.devicePixelRatio);
    }

    for (auto iterator = windows_.begin(); iterator != windows_.end();) {
        if (desiredNames.contains(iterator->first) && !staged.contains(iterator->first)) {
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
    current_ = snapshot;
    return Result<void>::success();
}

QWindow *DesktopWindowHost::window(std::string_view outputName) const noexcept {
    const auto found = windows_.find(outputName);
    return found == windows_.end() ? nullptr : found->second->view.get();
}

} // namespace prismdrake::shell::desktop
