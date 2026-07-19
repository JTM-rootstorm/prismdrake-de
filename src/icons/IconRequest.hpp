#pragma once

#include "DesktopFileId.hpp"

#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace prismdrake::icons {

inline constexpr int minimumIconLogicalSize = 8;
inline constexpr int maximumIconLogicalSize = 512;
inline constexpr qreal maximumIconDevicePixelRatio = 4.0;
inline constexpr qsizetype maximumIconSourceBytes = 4 * 1024 * 1024;
inline constexpr int maximumIconSourceDimension = 4096;
inline constexpr std::uint64_t maximumIconDecodedPixels = 16U * 1024U * 1024U;

struct IconRequest final {
    std::optional<launcher::DesktopFileId> desktopFileId;
    QString iconField;
    int logicalSize;
    qreal devicePixelRatio;
    std::uint64_t themeGeneration;
};

enum class IconResolutionSource : std::uint8_t { theme, absolutePath, genericFallback };

struct IconResolution final {
    QImage image;
    IconResolutionSource source;
    std::uint64_t themeGeneration;
};

} // namespace prismdrake::icons
