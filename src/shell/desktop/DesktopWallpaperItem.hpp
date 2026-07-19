#pragma once

#include "WallpaperLayout.hpp"
#include "WallpaperModel.hpp"

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

namespace prismdrake::shell::desktop {

/// Noninteractive C++ wallpaper renderer used by every desktop QML surface.
class DesktopWallpaperItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(DesktopWallpaper)

  public:
    explicit DesktopWallpaperItem(QQuickItem *parent = nullptr);

    void setSurface(std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper,
                    std::optional<prismdrake::desktop::WallpaperLayout> layout,
                    double devicePixelRatio);

    void paint(QPainter *painter) override;

    [[nodiscard]] std::uint64_t wallpaperGeneration() const noexcept;
    [[nodiscard]] bool paintsImage() const noexcept;

  private:
    std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper_;
    std::optional<prismdrake::desktop::WallpaperLayout> layout_;
    double device_pixel_ratio_{1.0};
};

} // namespace prismdrake::shell::desktop
