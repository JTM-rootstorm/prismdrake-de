#include "WallpaperLayout.hpp"

#include <algorithm>
#include <cmath>

namespace prismdrake::desktop {
namespace {

[[nodiscard]] QRectF centeredTarget(QSizeF output, QSizeF target) noexcept {
    return {(output.width() - target.width()) / 2.0, (output.height() - target.height()) / 2.0,
            target.width(), target.height()};
}

[[nodiscard]] bool validMode(config::WallpaperMode mode) noexcept {
    switch (mode) {
    case config::WallpaperMode::solid:
    case config::WallpaperMode::center:
    case config::WallpaperMode::fit:
    case config::WallpaperMode::fill:
    case config::WallpaperMode::stretch:
    case config::WallpaperMode::tile:
        return true;
    }
    return false;
}

[[nodiscard]] bool validRect(const QRectF &rect) noexcept {
    return rect.isValid() && std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
           std::isfinite(rect.width()) && std::isfinite(rect.height());
}

[[nodiscard]] std::optional<WallpaperLayout> checkedLayout(WallpaperLayout layout) {
    if (!validRect(layout.sourceRect) || !validRect(layout.targetRect) ||
        (layout.tiled &&
         (!layout.tileSize.isValid() || !std::isfinite(layout.tileSize.width()) ||
          !std::isfinite(layout.tileSize.height()) || !std::isfinite(layout.tileOrigin.x()) ||
          !std::isfinite(layout.tileOrigin.y())))) {
        return std::nullopt;
    }
    return layout;
}

} // namespace

std::optional<WallpaperLayout> calculateWallpaperLayout(config::WallpaperMode mode,
                                                        QSize sourcePixels,
                                                        QSizeF outputLogicalSize,
                                                        double devicePixelRatio) {
    const double sourceWidth = sourcePixels.width();
    const double sourceHeight = sourcePixels.height();
    const double outputWidth = outputLogicalSize.width();
    const double outputHeight = outputLogicalSize.height();
    if (!validMode(mode) || mode == config::WallpaperMode::solid || sourceWidth <= 0.0 ||
        sourceHeight <= 0.0 || outputWidth <= 0.0 || outputHeight <= 0.0 ||
        !std::isfinite(outputWidth) || !std::isfinite(outputHeight) ||
        outputWidth > maximumWallpaperOutputLogicalExtent ||
        outputHeight > maximumWallpaperOutputLogicalExtent || !std::isfinite(devicePixelRatio) ||
        devicePixelRatio < minimumWallpaperDevicePixelRatio ||
        devicePixelRatio > maximumWallpaperDevicePixelRatio) {
        return std::nullopt;
    }

    const QRectF completeSource{0.0, 0.0, sourceWidth, sourceHeight};
    const QRectF completeOutput{0.0, 0.0, outputWidth, outputHeight};
    switch (mode) {
    case config::WallpaperMode::center: {
        const QSizeF nativeLogicalSize{sourceWidth / devicePixelRatio,
                                       sourceHeight / devicePixelRatio};
        return checkedLayout(WallpaperLayout{
            completeSource, centeredTarget(outputLogicalSize, nativeLogicalSize), false, {}, {}});
    }
    case config::WallpaperMode::fit: {
        const double scale = std::min(outputWidth / sourceWidth, outputHeight / sourceHeight);
        const QSizeF target{sourceWidth * scale, sourceHeight * scale};
        return checkedLayout(WallpaperLayout{
            completeSource, centeredTarget(outputLogicalSize, target), false, {}, {}});
    }
    case config::WallpaperMode::fill: {
        const double scale = std::max(outputWidth / sourceWidth, outputHeight / sourceHeight);
        const double visibleWidth = outputWidth / scale;
        const double visibleHeight = outputHeight / scale;
        const QRectF visibleSource{(sourceWidth - visibleWidth) / 2.0,
                                   (sourceHeight - visibleHeight) / 2.0, visibleWidth,
                                   visibleHeight};
        return checkedLayout(WallpaperLayout{visibleSource, completeOutput, false, {}, {}});
    }
    case config::WallpaperMode::stretch:
        return checkedLayout(WallpaperLayout{completeSource, completeOutput, false, {}, {}});
    case config::WallpaperMode::tile: {
        const QSizeF nativeLogicalSize{sourceWidth / devicePixelRatio,
                                       sourceHeight / devicePixelRatio};
        return checkedLayout(WallpaperLayout{completeSource, completeOutput, true,
                                             nativeLogicalSize, QPointF{0.0, 0.0}});
    }
    case config::WallpaperMode::solid:
        break;
    }
    return std::nullopt;
}

} // namespace prismdrake::desktop
