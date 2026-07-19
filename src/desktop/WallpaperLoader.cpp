#include "WallpaperLoader.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prismdrake::desktop {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct Descriptor final {
    int value{-1};
    Descriptor() = default;
    explicit Descriptor(int descriptor) : value(descriptor) {}
    ~Descriptor() {
        if (value >= 0) {
            ::close(value);
        }
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    Descriptor(Descriptor &&other) noexcept : value(std::exchange(other.value, -1)) {}
    Descriptor &operator=(Descriptor &&) = delete;
};

struct SourceBytes final {
    WallpaperLoadStatus status;
    QByteArray bytes;
};

[[nodiscard]] bool containsUriScheme(const QString &source) {
    static const QRegularExpression scheme{QStringLiteral(R"(^[A-Za-z][A-Za-z0-9+.-]*:)")};
    return scheme.match(source).hasMatch();
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

[[nodiscard]] bool safePathComponents(const QString &path) {
    const QStringList components = path.split('/', Qt::KeepEmptyParts);
    for (qsizetype index = 0; index < components.size(); ++index) {
        const QString &component = components[index];
        if (component == "." || component == ".." || component.contains(QChar::Null)) {
            return false;
        }
        if (component.isEmpty() && index != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<QString> resolvedLocalPath(const WallpaperRequest &request) {
    const QString &source = request.source;
    if (source.isEmpty() || !boundedUnicode(source, 128) || source.startsWith("//") ||
        containsUriScheme(source) || !safePathComponents(source) ||
        QDir::cleanPath(source) != source) {
        return std::nullopt;
    }
    if (QDir::isAbsolutePath(source)) {
        return source;
    }
    if (request.sourceBaseDirectory.isEmpty() ||
        !QDir::isAbsolutePath(request.sourceBaseDirectory) ||
        QDir::cleanPath(request.sourceBaseDirectory) != request.sourceBaseDirectory) {
        return std::nullopt;
    }
    const QString resolved = request.sourceBaseDirectory == '/'
                                 ? request.sourceBaseDirectory + source
                                 : request.sourceBaseDirectory + '/' + source;
    return QDir::cleanPath(resolved) == resolved ? std::optional<QString>{resolved} : std::nullopt;
}

[[nodiscard]] SourceBytes readSource(const QString &path,
                                     const WallpaperDecoder::Cancelled &cancelled) {
    const QByteArray encodedPath = QFile::encodeName(path);
    if (encodedPath.isEmpty() || encodedPath.contains('\0')) {
        return {WallpaperLoadStatus::unsupported, {}};
    }
    Descriptor descriptor{
        ::open(encodedPath.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)};
    if (descriptor.value < 0) {
        return {errno == ENOENT ? WallpaperLoadStatus::unavailable
                                : WallpaperLoadStatus::unsupported,
                {}};
    }
    struct stat metadata = {};
    if (::fstat(descriptor.value, &metadata) != 0) {
        return {WallpaperLoadStatus::unavailable, {}};
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        return {WallpaperLoadStatus::unsupported, {}};
    }
    if (static_cast<std::uintmax_t>(metadata.st_size) >
        static_cast<std::uintmax_t>(maximumWallpaperSourceBytes)) {
        return {WallpaperLoadStatus::tooLarge, {}};
    }

    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(metadata.st_size));
    char buffer[64 * 1024];
    while (bytes.size() <= maximumWallpaperSourceBytes) {
        if (cancelled()) {
            return {WallpaperLoadStatus::invalid, {}};
        }
        const qsizetype remaining = maximumWallpaperSourceBytes + 1 - bytes.size();
        const auto requested = static_cast<std::size_t>(
            std::min<qsizetype>(remaining, static_cast<qsizetype>(sizeof(buffer))));
        const ssize_t count = ::read(descriptor.value, buffer, requested);
        if (count == 0) {
            return {WallpaperLoadStatus::ready, std::move(bytes)};
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {WallpaperLoadStatus::unavailable, {}};
        }
        bytes.append(buffer, static_cast<qsizetype>(count));
    }
    return {WallpaperLoadStatus::tooLarge, {}};
}

[[nodiscard]] bool validDecodedBounds(QSize size) noexcept {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
        size.width() > maximumWallpaperDimension || size.height() > maximumWallpaperDimension) {
        return false;
    }
    const auto width = static_cast<std::uint64_t>(size.width());
    const auto height = static_cast<std::uint64_t>(size.height());
    return width <= maximumWallpaperDecodedBytes / 4U / height;
}

class QtWallpaperDecoder final : public WallpaperDecoder {
  public:
    [[nodiscard]] WallpaperDecodeResult decode(const WallpaperRequest &request,
                                               const Cancelled &cancelled) const override {
        if (request.mode == config::WallpaperMode::solid || request.source.isEmpty()) {
            return {WallpaperLoadStatus::solid, {}};
        }
        const auto path = resolvedLocalPath(request);
        if (!path) {
            return {WallpaperLoadStatus::unsupported, {}};
        }
        SourceBytes source = readSource(*path, cancelled);
        if (source.status != WallpaperLoadStatus::ready || cancelled()) {
            return {source.status, {}};
        }

        QBuffer buffer{&source.bytes};
        if (!buffer.open(QIODevice::ReadOnly)) {
            return {WallpaperLoadStatus::invalid, {}};
        }
        QImageReader reader{&buffer};
        reader.setAutoTransform(true);
        reader.setDecideFormatFromContent(true);
        if (!reader.canRead()) {
            return {WallpaperLoadStatus::unsupported, {}};
        }
        if (!validDecodedBounds(reader.size())) {
            return {WallpaperLoadStatus::tooLarge, {}};
        }
        if (reader.supportsAnimation() || reader.imageCount() > 1 || cancelled()) {
            return {WallpaperLoadStatus::unsupported, {}};
        }
        QImage decoded = reader.read();
        if (cancelled()) {
            return {WallpaperLoadStatus::invalid, {}};
        }
        if (decoded.isNull() || !validDecodedBounds(decoded.size()) || decoded.sizeInBytes() < 0 ||
            static_cast<std::uint64_t>(decoded.sizeInBytes()) > maximumWallpaperDecodedBytes) {
            return {WallpaperLoadStatus::invalid, {}};
        }
        return {WallpaperLoadStatus::ready, std::make_shared<const QImage>(std::move(decoded))};
    }
};

} // namespace

struct WallpaperLoader::State final {
    struct Pending final {
        std::uint64_t ownerEpoch;
        WallpaperRequest request;
        QPointer<QObject> context;
        Completion completion;
    };

    std::mutex mutex;
    std::shared_ptr<const WallpaperDecoder> decoder;
    std::unique_ptr<Pending> pending;
    std::shared_ptr<const WallpaperSnapshot> current;
    std::shared_ptr<const WallpaperSnapshot> previous;
    std::uint64_t ownerEpoch{1U};
    std::uint64_t latestGeneration{0U};
    bool workerScheduled{false};
    bool stopped{false};
};

std::shared_ptr<const WallpaperDecoder> makeQtWallpaperDecoder() {
    static std::once_flag allocationLimit;
    std::call_once(allocationLimit, [] {
        const int currentLimit = QImageReader::allocationLimit();
        if (currentLimit <= 0 || currentLimit > 256) {
            QImageReader::setAllocationLimit(256);
        }
    });
    return std::make_shared<QtWallpaperDecoder>();
}

WallpaperLoader::WallpaperLoader(std::shared_ptr<const WallpaperDecoder> decoder)
    : state_(std::make_shared<State>()) {
    state_->decoder = decoder ? std::move(decoder) : makeQtWallpaperDecoder();
}

WallpaperLoader::~WallpaperLoader() {
    if (state_) {
        const std::scoped_lock lock{state_->mutex};
        state_->stopped = true;
        state_->pending.reset();
    }
}

Result<void> WallpaperLoader::load(WallpaperRequest request, QObject *context,
                                   Completion completion) {
    if (!state_ || context == nullptr || !completion || request.generation == 0U ||
        !request.backgroundColor.isValid() || !validMode(request.mode) ||
        !boundedUnicode(request.source, 128) ||
        !boundedUnicode(request.sourceBaseDirectory, 4096) ||
        QThread::currentThread() != context->thread()) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The immutable wallpaper request is invalid.",
             "Submit bounded settings from the receiving QObject thread."});
    }

    const auto state = state_;
    bool startWorker = false;
    {
        const std::scoped_lock lock{state->mutex};
        if (state->stopped || request.generation <= state->latestGeneration) {
            return Result<void>::failure(
                {ErrorCode::cancelled, "The wallpaper generation is stale.",
                 "Submit a generation newer than the current wallpaper settings."});
        }
        state->latestGeneration = request.generation;
        state->pending = std::make_unique<State::Pending>(
            State::Pending{state->ownerEpoch, std::move(request), QPointer<QObject>{context},
                           std::move(completion)});
        if (!state->workerScheduled) {
            state->workerScheduled = true;
            startWorker = true;
        }
    }
    if (!startWorker) {
        return Result<void>::success();
    }

    QThreadPool::globalInstance()->start([state] {
        for (;;) {
            std::unique_ptr<State::Pending> work;
            {
                const std::scoped_lock lock{state->mutex};
                if (state->stopped || !state->pending) {
                    state->workerScheduled = false;
                    return;
                }
                work = std::move(state->pending);
            }
            const std::uint64_t generation = work->request.generation;
            const std::uint64_t ownerEpoch = work->ownerEpoch;
            const auto cancelled = [state, ownerEpoch, generation] {
                const std::scoped_lock lock{state->mutex};
                return state->stopped || state->ownerEpoch != ownerEpoch ||
                       state->latestGeneration != generation;
            };
            WallpaperDecodeResult decoded = state->decoder->decode(work->request, cancelled);
            if (cancelled()) {
                continue;
            }
            if (decoded.status != WallpaperLoadStatus::ready) {
                decoded.image.reset();
            } else if (!decoded.image || decoded.image->isNull() ||
                       !validDecodedBounds(decoded.image->size()) ||
                       decoded.image->sizeInBytes() < 0 ||
                       static_cast<std::uint64_t>(decoded.image->sizeInBytes()) >
                           maximumWallpaperDecodedBytes) {
                decoded.status = WallpaperLoadStatus::invalid;
                decoded.image.reset();
            }
            const QSize sourceSize = decoded.image ? decoded.image->size() : QSize{};
            auto snapshot = std::make_shared<const WallpaperSnapshot>(
                WallpaperSnapshot{generation, work->request.backgroundColor, work->request.mode,
                                  decoded.status, sourceSize, std::move(decoded.image)});
            {
                const std::scoped_lock lock{state->mutex};
                if (state->stopped || state->ownerEpoch != ownerEpoch ||
                    state->latestGeneration != generation) {
                    continue;
                }
                state->previous = std::move(state->current);
                state->current = snapshot;
            }
            if (!work->context) {
                continue;
            }
            QMetaObject::invokeMethod(
                work->context,
                [state, ownerEpoch, guard = work->context, completion = std::move(work->completion),
                 snapshot = std::move(snapshot)]() mutable {
                    if (!guard) {
                        return;
                    }
                    {
                        const std::scoped_lock lock{state->mutex};
                        if (state->stopped || state->ownerEpoch != ownerEpoch ||
                            state->latestGeneration != snapshot->generation ||
                            state->current != snapshot) {
                            return;
                        }
                    }
                    completion(std::move(snapshot));
                },
                Qt::QueuedConnection);
        }
    });
    return Result<void>::success();
}

void WallpaperLoader::beginSettingsOwnerEpoch() noexcept {
    if (!state_) {
        return;
    }
    const std::scoped_lock lock{state_->mutex};
    if (state_->stopped) {
        return;
    }
    if (state_->ownerEpoch == std::numeric_limits<std::uint64_t>::max()) {
        state_->stopped = true;
        state_->pending.reset();
        return;
    }
    ++state_->ownerEpoch;
    state_->latestGeneration = 0U;
    state_->pending.reset();
}

std::shared_ptr<const WallpaperSnapshot> WallpaperLoader::current() const {
    if (!state_) {
        return {};
    }
    const std::scoped_lock lock{state_->mutex};
    return state_->current;
}

} // namespace prismdrake::desktop
