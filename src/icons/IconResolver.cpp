#include "IconResolver.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

void initializePrismdrakeIconResources() { Q_INIT_RESOURCE(prismdrake_icons); }

namespace prismdrake::icons {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::size_t maximumCachedIcons = 128U;
constexpr std::uint64_t maximumCachedIconBytes = 64U * 1024U * 1024U;
constexpr std::size_t maximumOutstandingIconRequests = 32U;
constexpr qsizetype maximumThemeIndexBytes = 1024 * 1024;
constexpr std::size_t maximumThemeDirectories = 256U;
constexpr std::size_t maximumThemeInheritance = 8U;
constexpr auto fallbackResource = ":/org/prismdrake/icons/prismdrake-generic.svg";

struct IconCacheKey final {
    std::string desktopFileId;
    std::string iconField;
    int logicalSize;
    int devicePixelRatioMilli;
    std::uint64_t themeGeneration;

    friend auto operator<=>(const IconCacheKey &, const IconCacheKey &) = default;
};

struct ThemeLookupSnapshot final {
    bool qtThemeHasIcon;
    QString themeName;
    QString fallbackThemeName;
    QStringList searchPaths;
};

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure({code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool safeThemeName(const QString &name) noexcept {
    if (name.isEmpty() || name.size() > 255 || name.startsWith(QLatin1Char('.'))) {
        return false;
    }
    for (const QChar character : name) {
        const ushort value = character.unicode();
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validImageBounds(const QSize &size) noexcept {
    return size.isValid() && size.width() > 0 && size.height() > 0 &&
           size.width() <= maximumIconSourceDimension &&
           size.height() <= maximumIconSourceDimension &&
           static_cast<std::uint64_t>(size.width()) <=
               maximumIconDecodedPixels / static_cast<std::uint64_t>(size.height());
}

[[nodiscard]] std::optional<QByteArray> readRegularNoFollow(const QString &path,
                                                            qsizetype maximumBytes) {
    const QByteArray encodedPath = QFile::encodeName(path);
    if (encodedPath.isEmpty() || encodedPath.contains('\0')) {
        return std::nullopt;
    }
    const int raw = ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (raw < 0) {
        return std::nullopt;
    }
    struct Descriptor final {
        int value;
        ~Descriptor() { ::close(value); }
    } descriptor{raw};
    struct stat metadata = {};
    if (::fstat(raw, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > static_cast<std::uintmax_t>(maximumBytes)) {
        return std::nullopt;
    }
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(metadata.st_size));
    char buffer[16 * 1024];
    while (bytes.size() <= maximumBytes) {
        const qsizetype remaining = maximumBytes + 1 - bytes.size();
        const auto requested = static_cast<std::size_t>(
            std::min<qsizetype>(remaining, static_cast<qsizetype>(sizeof(buffer))));
        const ssize_t count = ::read(raw, buffer, requested);
        if (count == 0) {
            return bytes;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        bytes.append(buffer, static_cast<qsizetype>(count));
    }
    return std::nullopt;
}

[[nodiscard]] QImage decodeBytes(const QByteArray &bytes, const QSize &pixelSize) {
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    reader.setDecideFormatFromContent(true);
    if (!validImageBounds(reader.size()) || reader.supportsAnimation() || reader.imageCount() > 1) {
        return {};
    }
    reader.setScaledSize(pixelSize);
    QImage image = reader.read();
    if (image.isNull() || image.size() != pixelSize || !validImageBounds(image.size()) ||
        image.sizeInBytes() < 0 ||
        static_cast<std::uint64_t>(image.sizeInBytes()) > maximumIconDecodedPixels * 4U) {
        return {};
    }
    return image;
}

[[nodiscard]] QImage decodeResource(const QString &resource, const QSize &pixelSize) {
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.read(maximumIconSourceBytes + 1);
    return bytes.size() <= maximumIconSourceBytes ? decodeBytes(bytes, pixelSize) : QImage{};
}

[[nodiscard]] QImage decodeAbsolutePath(const QString &path, const QSize &pixelSize) {
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path) ||
        QDir::cleanPath(path) != path) {
        return {};
    }
    auto bytes = readRegularNoFollow(path, maximumIconSourceBytes);
    return bytes ? decodeBytes(*bytes, pixelSize) : QImage{};
}

[[nodiscard]] QImage drawOriginalFallback(const QSize &pixelSize) {
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal width = pixelSize.width();
    const qreal height = pixelSize.height();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{"#182033"});
    painter.drawPolygon(QPolygonF{{width * 0.5, height * 0.05},
                                  {width * 0.9, height * 0.28},
                                  {width * 0.9, height * 0.72},
                                  {width * 0.5, height * 0.95},
                                  {width * 0.1, height * 0.72},
                                  {width * 0.1, height * 0.28}});
    painter.setBrush(QColor{"#75d7ff"});
    painter.drawPolygon(QPolygonF{{width * 0.5, height * 0.12},
                                  {width * 0.82, height * 0.31},
                                  {width * 0.5, height * 0.45},
                                  {width * 0.18, height * 0.31}});
    painter.setBrush(QColor{"#936dff"});
    painter.drawPolygon(QPolygonF{{width * 0.18, height * 0.36},
                                  {width * 0.47, height * 0.5},
                                  {width * 0.47, height * 0.86},
                                  {width * 0.18, height * 0.69}});
    painter.setBrush(QColor{"#e868c8"});
    painter.drawPolygon(QPolygonF{{width * 0.82, height * 0.36},
                                  {width * 0.53, height * 0.5},
                                  {width * 0.53, height * 0.86},
                                  {width * 0.82, height * 0.69}});
    return image;
}

enum class ThemeDirectoryType : std::uint8_t { fixed, scalable, threshold };

struct ThemeDirectory final {
    QString name;
    int size{0};
    int scale{1};
    int minimumSize{0};
    int maximumSize{0};
    int threshold{2};
    ThemeDirectoryType type{ThemeDirectoryType::threshold};
    bool valid{true};
};

struct ThemeIndex final {
    std::vector<ThemeDirectory> directories;
    QStringList inherits;
};

[[nodiscard]] bool safeRelativeThemeDirectory(const QString &directory) {
    if (directory.isEmpty() || directory.size() > 4096 || directory.startsWith('/') ||
        QDir::cleanPath(directory) != directory) {
        return false;
    }
    const QStringList components = directory.split('/', Qt::KeepEmptyParts);
    return components.size() <= 64 && std::ranges::all_of(components, [](const QString &component) {
               return !component.isEmpty() && component != "." && component != ".." &&
                      !component.contains(QChar::Null);
           });
}

[[nodiscard]] std::optional<int> boundedPositiveInteger(const QByteArray &value,
                                                        int maximum = 4096) {
    bool valid = false;
    const int parsed = value.trimmed().toInt(&valid);
    return valid && parsed > 0 && parsed <= maximum ? std::optional<int>{parsed} : std::nullopt;
}

[[nodiscard]] std::optional<int> boundedNonnegativeInteger(const QByteArray &value,
                                                           int maximum = 4096) {
    bool valid = false;
    const int parsed = value.trimmed().toInt(&valid);
    return valid && parsed >= 0 && parsed <= maximum ? std::optional<int>{parsed} : std::nullopt;
}

[[nodiscard]] ThemeIndex parseThemeIndex(const QString &path) {
    auto bytes = readRegularNoFollow(path, maximumThemeIndexBytes);
    if (!bytes) {
        return {};
    }
    ThemeIndex result;
    QStringList declaredDirectories;
    std::map<QString, ThemeDirectory> directorySections;
    QString currentGroup;
    std::size_t lineCount = 0U;
    for (QByteArray line : bytes->split('\n')) {
        if (++lineCount > 4096U) {
            return {};
        }
        line = line.trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            currentGroup = QString::fromUtf8(line.sliced(1, line.size() - 2));
            continue;
        }
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (equals <= 0) {
            continue;
        }
        const QByteArray key = line.first(equals).trimmed();
        const QByteArray value = line.sliced(equals + 1).trimmed();
        if (currentGroup == "Icon Theme") {
            const QStringList values = QString::fromUtf8(value).split(',', Qt::SkipEmptyParts);
            if (key == "Directories") {
                declaredDirectories = values.mid(0, maximumThemeDirectories);
            } else if (key == "Inherits") {
                result.inherits = values.mid(0, maximumThemeInheritance);
            }
            continue;
        }
        if (!safeRelativeThemeDirectory(currentGroup)) {
            continue;
        }
        auto &directory = directorySections[currentGroup];
        directory.name = currentGroup;
        if (key == "Size") {
            directory.size = boundedPositiveInteger(value).value_or(0);
        } else if (key == "Scale") {
            directory.scale = boundedPositiveInteger(value, 8).value_or(0);
        } else if (key == "MinSize") {
            directory.minimumSize = boundedPositiveInteger(value).value_or(0);
        } else if (key == "MaxSize") {
            directory.maximumSize = boundedPositiveInteger(value).value_or(0);
        } else if (key == "Threshold") {
            directory.threshold = boundedNonnegativeInteger(value).value_or(-1);
        } else if (key == "Type") {
            if (value == "Fixed") {
                directory.type = ThemeDirectoryType::fixed;
            } else if (value == "Scalable") {
                directory.type = ThemeDirectoryType::scalable;
            } else if (value == "Threshold") {
                directory.type = ThemeDirectoryType::threshold;
            } else {
                directory.valid = false;
            }
        }
    }
    for (QString name : declaredDirectories) {
        name = name.trimmed();
        const auto found = directorySections.find(name);
        if (!safeRelativeThemeDirectory(name) || found == directorySections.end()) {
            continue;
        }
        ThemeDirectory directory = found->second;
        if (!directory.valid || directory.size <= 0 || directory.scale <= 0 ||
            directory.threshold < 0) {
            continue;
        }
        if (directory.minimumSize == 0) {
            directory.minimumSize = directory.size;
        }
        if (directory.maximumSize == 0) {
            directory.maximumSize = directory.size;
        }
        if (directory.minimumSize > directory.maximumSize) {
            continue;
        }
        result.directories.push_back(std::move(directory));
    }
    return result;
}

struct ThemeCandidate final {
    bool exact;
    double distance;
    double scaleDistance;
    std::size_t themeOrder;
    std::size_t directoryOrder;
    std::size_t extensionOrder;
    QString path;
};

[[nodiscard]] std::pair<bool, double> directoryDistance(const ThemeDirectory &directory,
                                                        int logicalSize, qreal dpr) {
    const double physical = static_cast<double>(logicalSize) * dpr;
    const double scale = directory.scale;
    double minimum = directory.size * scale;
    double maximum = minimum;
    if (directory.type == ThemeDirectoryType::scalable) {
        minimum = directory.minimumSize * scale;
        maximum = directory.maximumSize * scale;
    } else if (directory.type == ThemeDirectoryType::threshold) {
        minimum = std::max(1, directory.size - directory.threshold) * scale;
        maximum = (directory.size + directory.threshold) * scale;
    }
    const bool exactScale = std::abs(static_cast<double>(directory.scale) - dpr) < 0.001;
    const bool exactLogical =
        directory.type == ThemeDirectoryType::fixed
            ? directory.size == logicalSize
            : logicalSize >= (directory.type == ThemeDirectoryType::scalable
                                  ? directory.minimumSize
                                  : std::max(1, directory.size - directory.threshold)) &&
                  logicalSize <= (directory.type == ThemeDirectoryType::scalable
                                      ? directory.maximumSize
                                      : directory.size + directory.threshold);
    const double distance = physical < minimum   ? minimum - physical
                            : physical > maximum ? physical - maximum
                                                 : 0.0;
    return {exactScale && exactLogical, distance};
}

[[nodiscard]] QImage resolveThemeImage(const ThemeLookupSnapshot &theme, const QString &iconName,
                                       int logicalSize, qreal dpr, const QSize &pixelSize) {
    if (!theme.qtThemeHasIcon) {
        return {};
    }
    QStringList pending;
    for (const QString &candidate :
         {theme.themeName, theme.fallbackThemeName, QStringLiteral("hicolor")}) {
        if (!candidate.isEmpty() && !pending.contains(candidate)) {
            pending.push_back(candidate);
        }
    }
    std::set<std::string> visited;
    std::vector<ThemeCandidate> candidates;
    std::size_t themeOrder = 0U;
    for (qsizetype cursor = 0; cursor < pending.size() && visited.size() < maximumThemeInheritance;
         ++cursor) {
        const QString themeName = pending[cursor].trimmed();
        if (!safeThemeName(themeName) || !visited.insert(themeName.toStdString()).second) {
            continue;
        }
        for (const QString &root : theme.searchPaths) {
            if (!QDir::isAbsolutePath(root) || root.startsWith(':')) {
                continue;
            }
            const QString themeRoot = QDir::cleanPath(root + '/' + themeName);
            const ThemeIndex index = parseThemeIndex(themeRoot + "/index.theme");
            for (std::size_t directoryOrder = 0U; directoryOrder < index.directories.size();
                 ++directoryOrder) {
                const auto &directory = index.directories[directoryOrder];
                const auto [exact, distance] = directoryDistance(directory, logicalSize, dpr);
                std::size_t extensionOrder = 0U;
                for (const QString &extension :
                     {QStringLiteral(".png"), QStringLiteral(".svg"), QStringLiteral(".xpm")}) {
                    candidates.push_back(
                        {exact, distance, std::abs(static_cast<double>(directory.scale) - dpr),
                         themeOrder, directoryOrder, extensionOrder++,
                         themeRoot + '/' + directory.name + '/' + iconName + extension});
                }
            }
            for (QString inherited : index.inherits) {
                inherited = inherited.trimmed();
                if (safeThemeName(inherited) && !pending.contains(inherited) &&
                    pending.size() < static_cast<qsizetype>(maximumThemeDirectories)) {
                    pending.push_back(inherited);
                }
            }
        }
        ++themeOrder;
    }
    std::ranges::sort(candidates, [](const ThemeCandidate &left, const ThemeCandidate &right) {
        return std::tuple{!left.exact,     left.distance,       left.scaleDistance,
                          left.themeOrder, left.directoryOrder, left.extensionOrder} <
               std::tuple{!right.exact,     right.distance,       right.scaleDistance,
                          right.themeOrder, right.directoryOrder, right.extensionOrder};
    });
    for (const auto &candidate : candidates) {
        QImage image = decodeAbsolutePath(candidate.path, pixelSize);
        if (!image.isNull()) {
            return image;
        }
    }
    return {};
}

[[nodiscard]] IconCacheKey cacheKey(const IconRequest &request) {
    return {request.desktopFileId ? request.desktopFileId->value() : std::string{},
            request.iconField.toStdString(), request.logicalSize,
            static_cast<int>(std::lround(request.devicePixelRatio * 1000.0)),
            request.themeGeneration};
}

[[nodiscard]] IconResolution resolveSynchronously(const IconRequest &request,
                                                  const ThemeLookupSnapshot &theme) {
    const int pixels = static_cast<int>(
        std::ceil(static_cast<qreal>(request.logicalSize) * request.devicePixelRatio));
    const QSize pixelSize{pixels, pixels};
    QImage image;
    IconResolutionSource source = IconResolutionSource::genericFallback;
    if (QDir::isAbsolutePath(request.iconField)) {
        image = decodeAbsolutePath(request.iconField, pixelSize);
        source = image.isNull() ? source : IconResolutionSource::absolutePath;
    } else if (safeThemeName(request.iconField)) {
        image = resolveThemeImage(theme, request.iconField, request.logicalSize,
                                  request.devicePixelRatio, pixelSize);
        source = image.isNull() ? source : IconResolutionSource::theme;
    }
    if (image.isNull()) {
        image = decodeResource(QString::fromLatin1(fallbackResource), pixelSize);
        if (image.isNull()) {
            image = drawOriginalFallback(pixelSize);
        }
        source = IconResolutionSource::genericFallback;
    }
    image.setDevicePixelRatio(request.devicePixelRatio);
    return {std::move(image), source, request.themeGeneration};
}

} // namespace

struct IconResolver::State final {
    std::mutex mutex;
    std::map<IconCacheKey, IconResolution> cache;
    std::uint64_t cachedBytes{0U};
    std::size_t outstanding{0U};
    std::size_t maximumOutstanding{maximumOutstandingIconRequests};
};

IconResolver::IconResolver(std::size_t maximumOutstandingRequests)
    : state_(std::make_shared<State>()) {
    initializePrismdrakeIconResources();
    state_->maximumOutstanding =
        std::clamp(maximumOutstandingRequests, std::size_t{1U}, maximumOutstandingIconRequests);
}

IconResolver::~IconResolver() = default;
IconResolver::IconResolver(IconResolver &&) noexcept = default;
IconResolver &IconResolver::operator=(IconResolver &&) noexcept = default;

Result<void> IconResolver::resolve(IconRequest request, QObject *context,
                                   Completion completion) const {
    if (!state_ || context == nullptr || !completion ||
        request.logicalSize < minimumIconLogicalSize ||
        request.logicalSize > maximumIconLogicalSize || !std::isfinite(request.devicePixelRatio) ||
        request.devicePixelRatio <= 0.0 || request.devicePixelRatio > maximumIconDevicePixelRatio ||
        request.themeGeneration == 0U ||
        request.iconField.size() > static_cast<qsizetype>(launcher::maximumDesktopFileIdBytes)) {
        return failure<void>(ErrorCode::invalid_argument, "The immutable icon request is invalid.",
                             "Use bounded size, scale, metadata, and a nonzero theme generation.");
    }
    if (QThread::currentThread() != context->thread()) {
        return failure<void>(ErrorCode::invalid_argument,
                             "The icon adapter context thread is invalid.",
                             "Submit icon requests from the receiving QObject thread.");
    }

    ThemeLookupSnapshot theme{false, QIcon::themeName(), QIcon::fallbackThemeName(),
                              QIcon::themeSearchPaths()};
    if (safeThemeName(request.iconField) && !QDir::isAbsolutePath(request.iconField)) {
        theme.qtThemeHasIcon = !QIcon::fromTheme(request.iconField).isNull();
    }

    const auto state = state_;
    const auto key = cacheKey(request);
    {
        const std::scoped_lock lock{state->mutex};
        if (state->outstanding >= state->maximumOutstanding) {
            return failure<void>(ErrorCode::too_large, "The bounded icon work queue is full.",
                                 "Wait for an outstanding icon request to complete.");
        }
        ++state->outstanding;
    }
    QPointer<QObject> guard{context};
    QThreadPool::globalInstance()->start([state, key, request = std::move(request),
                                          theme = std::move(theme), guard,
                                          completion = std::move(completion)]() mutable {
        IconResolution resolution;
        {
            const std::scoped_lock lock{state->mutex};
            const auto found = state->cache.find(key);
            if (found != state->cache.end()) {
                resolution = found->second;
            }
        }
        if (resolution.image.isNull()) {
            resolution = resolveSynchronously(request, theme);
            const std::scoped_lock lock{state->mutex};
            const auto existing = state->cache.find(key);
            if (existing != state->cache.end()) {
                state->cachedBytes -=
                    static_cast<std::uint64_t>(existing->second.image.sizeInBytes());
                state->cache.erase(existing);
            }
            const auto cost = static_cast<std::uint64_t>(resolution.image.sizeInBytes());
            while (!state->cache.empty() && (state->cache.size() >= maximumCachedIcons ||
                                             state->cachedBytes + cost > maximumCachedIconBytes)) {
                state->cachedBytes -=
                    static_cast<std::uint64_t>(state->cache.begin()->second.image.sizeInBytes());
                state->cache.erase(state->cache.begin());
            }
            state->cache.emplace(key, resolution);
            state->cachedBytes += cost;
        }
        if (guard) {
            QMetaObject::invokeMethod(
                guard,
                [guard, completion = std::move(completion),
                 resolution = std::move(resolution)]() mutable {
                    if (guard) {
                        completion(std::move(resolution));
                    }
                },
                Qt::QueuedConnection);
        }
        const std::scoped_lock lock{state->mutex};
        --state->outstanding;
    });
    return Result<void>::success();
}

} // namespace prismdrake::icons
