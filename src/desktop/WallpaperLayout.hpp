#pragma once

#include "Configuration.hpp"

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>

#include <optional>

namespace prismdrake::desktop {

inline constexpr double minimumWallpaperDevicePixelRatio = 0.25;
inline constexpr double maximumWallpaperDevicePixelRatio = 8.0;
inline constexpr double maximumWallpaperOutputLogicalExtent = 4'294'967'295.0;

/// Deterministic paint geometry in output-local logical coordinates.
struct WallpaperLayout final {
    const QRectF sourceRect;
    const QRectF targetRect;
    const bool tiled;
    const QSizeF tileSize;
    const QPointF tileOrigin;
};

/// Returns no image layout for invalid geometry or solid mode. Center and tile map one decoded
/// source pixel to one output device pixel; fit, fill, and stretch use the complete logical output.
[[nodiscard]] std::optional<WallpaperLayout>
calculateWallpaperLayout(config::WallpaperMode mode, QSize sourcePixels, QSizeF outputLogicalSize,
                         double devicePixelRatio = 1.0);

} // namespace prismdrake::desktop
