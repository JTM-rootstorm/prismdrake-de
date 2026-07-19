#pragma once

#include "Configuration.hpp"
#include "Result.hpp"

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

namespace prismdrake::desktop {

inline constexpr qsizetype maximumWallpaperSourceBytes = 64 * 1024 * 1024;
inline constexpr int maximumWallpaperDimension = 16'384;
inline constexpr std::uint64_t maximumWallpaperDecodedBytes = 256U * 1024U * 1024U;

enum class WallpaperLoadStatus : std::uint8_t {
    solid,
    ready,
    unavailable,
    invalid,
    unsupported,
    tooLarge
};

/// Immutable input for one generation. Relative sources require an explicit absolute base.
struct WallpaperRequest final {
    const QString source;
    const QString sourceBaseDirectory;
    const QColor backgroundColor;
    const config::WallpaperMode mode;
    const std::uint64_t generation;
};

/// Immutable decoded state shared by every output surface for one settings generation.
struct WallpaperSnapshot final {
    const std::uint64_t generation;
    const QColor backgroundColor;
    const config::WallpaperMode mode;
    const WallpaperLoadStatus status;
    const QSize sourceSize;
    const std::shared_ptr<const QImage> image;

    [[nodiscard]] bool hasImage() const noexcept {
        return status == WallpaperLoadStatus::ready && image && !image->isNull();
    }
};

/// Converts the frozen configuration value without resolving or reading the source path.
[[nodiscard]] foundation::Result<WallpaperRequest>
makeWallpaperRequest(const config::Desktop &configuration, QString sourceBaseDirectory,
                     std::uint64_t generation);

} // namespace prismdrake::desktop
