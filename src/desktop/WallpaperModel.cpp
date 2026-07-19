#include "WallpaperModel.hpp"

#include <QDir>

#include <utility>

namespace prismdrake::desktop {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] bool validColor(const std::string &value) noexcept {
    if (value.size() != 7U || value.front() != '#') {
        return false;
    }
    for (std::size_t index = 1U; index < value.size(); ++index) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') || (character >= 'A' && character <= 'F') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool boundedUnicode(const QString &value, qsizetype maximumCodePoints) noexcept {
    qsizetype codePoints = 0;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value[index];
        if (character.isHighSurrogate()) {
            if (++index >= value.size() || !value[index].isLowSurrogate()) {
                return false;
            }
        } else if (character.isLowSurrogate() || character.isNull()) {
            return false;
        }
        if (++codePoints > maximumCodePoints) {
            return false;
        }
    }
    return true;
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

} // namespace

Result<WallpaperRequest> makeWallpaperRequest(const config::Desktop &configuration,
                                              QString sourceBaseDirectory,
                                              std::uint64_t generation) {
    if (generation == 0U || !validColor(configuration.backgroundColor) ||
        !validMode(configuration.wallpaperMode)) {
        return Result<WallpaperRequest>::failure(
            {ErrorCode::invalid_argument, "The wallpaper settings generation is invalid.",
             "Use a nonzero generation and a validated RGB background color."});
    }
    const QString source = QString::fromUtf8(configuration.wallpaperSource);
    if (source.toUtf8().toStdString() != configuration.wallpaperSource ||
        !boundedUnicode(source, 128)) {
        return Result<WallpaperRequest>::failure({ErrorCode::too_large,
                                                  "The wallpaper source metadata is invalid.",
                                                  "Use an empty source or a bounded local path."});
    }
    if (!boundedUnicode(sourceBaseDirectory, 4096) ||
        (!sourceBaseDirectory.isEmpty() &&
         (!QDir::isAbsolutePath(sourceBaseDirectory) ||
          QDir::cleanPath(sourceBaseDirectory) != sourceBaseDirectory))) {
        return Result<WallpaperRequest>::failure(
            {ErrorCode::invalid_argument, "The wallpaper source base is invalid.",
             "Use an empty base or a clean absolute configuration directory."});
    }
    return Result<WallpaperRequest>::success(
        {source, std::move(sourceBaseDirectory),
         QColor{QString::fromLatin1(configuration.backgroundColor)}, configuration.wallpaperMode,
         generation});
}

} // namespace prismdrake::desktop
