#include "DesktopWallpaperItem.hpp"

#include <QBrush>
#include <QPainter>
#include <QPixmap>
#include <QTransform>

#include <utility>

namespace prismdrake::shell::desktop {

DesktopWallpaperItem::DesktopWallpaperItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    setFlag(ItemAcceptsInputMethod, false);
    setAntialiasing(false);
    setOpaquePainting(true);
}

void DesktopWallpaperItem::setSurface(
    std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper,
    std::optional<prismdrake::desktop::WallpaperLayout> layout, double devicePixelRatio) {
    wallpaper_ = std::move(wallpaper);
    layout_.reset();
    if (layout) {
        layout_.emplace(*layout);
    }
    device_pixel_ratio_ = devicePixelRatio;
    update();
}

void DesktopWallpaperItem::paint(QPainter *painter) {
    if (painter == nullptr || !wallpaper_) {
        return;
    }
    painter->fillRect(boundingRect(), wallpaper_->backgroundColor);
    if (!wallpaper_->hasImage() || !layout_) {
        return;
    }

    painter->save();
    painter->setClipRect(boundingRect());
    if (layout_->tiled) {
        QBrush wallpaperBrush{QPixmap::fromImage(*wallpaper_->image)};
        wallpaperBrush.setTransform(
            QTransform::fromScale(1.0 / device_pixel_ratio_, 1.0 / device_pixel_ratio_));
        painter->setBrushOrigin(layout_->tileOrigin);
        painter->fillRect(layout_->targetRect, wallpaperBrush);
    } else {
        const bool pixelExact = wallpaper_->mode == config::WallpaperMode::center;
        painter->setRenderHint(QPainter::SmoothPixmapTransform, !pixelExact);
        painter->drawImage(layout_->targetRect, *wallpaper_->image, layout_->sourceRect);
    }
    painter->restore();
}

std::uint64_t DesktopWallpaperItem::wallpaperGeneration() const noexcept {
    return wallpaper_ ? wallpaper_->generation : 0U;
}

bool DesktopWallpaperItem::paintsImage() const noexcept {
    return wallpaper_ && wallpaper_->hasImage() && layout_.has_value();
}

} // namespace prismdrake::shell::desktop
