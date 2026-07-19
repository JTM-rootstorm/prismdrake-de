#include "WallpaperLoader.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImageIOHandler>
#include <QImageWriter>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::desktop {
namespace {

static_assert(!std::is_move_constructible_v<WallpaperLoader>);
static_assert(!std::is_move_assignable_v<WallpaperLoader>);

class TemporaryWallpapers final {
  public:
    TemporaryWallpapers() {
        std::string pattern = "/tmp/prismdrake-wallpaper-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create the wallpaper fixture directory."};
        }
        root_ = created;
    }

    ~TemporaryWallpapers() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] QString root() const { return QString::fromStdString(root_.string()); }

    [[nodiscard]] QString path(const QString &relative) const {
        return QString::fromStdString((root_ / relative.toStdString()).string());
    }

    QString write(const QString &relative, const QByteArray &bytes) const {
        const QString destination = path(relative);
        QDir{}.mkpath(QFileInfo{destination}.absolutePath());
        QFile file{destination};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size()) {
            throw std::runtime_error{"Could not write the wallpaper fixture."};
        }
        return destination;
    }

    QString writeImage(const QString &relative, const QImage &image,
                       const QByteArray &format = "PNG") const {
        QByteArray bytes;
        QBuffer buffer{&bytes};
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer{&buffer, format};
        if (!writer.write(image)) {
            throw std::runtime_error{"Could not encode the wallpaper fixture."};
        }
        return write(relative, bytes);
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] WallpaperRequest request(QString source, QString base, std::uint64_t generation,
                                       config::WallpaperMode mode = config::WallpaperMode::fill) {
    return {std::move(source), std::move(base), QColor{"#182033"}, mode, generation};
}

[[nodiscard]] std::shared_ptr<const WallpaperSnapshot>
loadAndWait(WallpaperLoader &loader, WallpaperRequest wallpaperRequest) {
    QObject context;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::shared_ptr<const WallpaperSnapshot> snapshot;
    bool callbackOnContextThread = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto queued = loader.load(std::move(wallpaperRequest), &context,
                                    [&](std::shared_ptr<const WallpaperSnapshot> published) {
                                        snapshot = std::move(published);
                                        callbackOnContextThread =
                                            QThread::currentThread() == context.thread();
                                        loop.quit();
                                    });
    EXPECT_TRUE(queued);
    timeout.start(5000);
    loop.exec();
    EXPECT_TRUE(snapshot);
    EXPECT_TRUE(callbackOnContextThread);
    return snapshot;
}

[[nodiscard]] QByteArray allocationBombBmp() {
    QByteArray bytes(54, '\0');
    auto write16 = [&](int offset, std::uint16_t value) {
        bytes[offset] = static_cast<char>(value & 0xffU);
        bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
    };
    auto write32 = [&](int offset, std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xffU);
        }
    };
    bytes[0] = 'B';
    bytes[1] = 'M';
    write32(2, 54U);
    write32(10, 54U);
    write32(14, 40U);
    write32(18, 16'384U);
    write32(22, 16'384U);
    write16(26, 1U);
    write16(28, 32U);
    return bytes;
}

TEST(WallpaperLoaderTest, EmptySourceAndSolidModePublishSolidFallback) {
    WallpaperLoader loader;
    const auto empty = loadAndWait(loader, request({}, {}, 1U));
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, WallpaperLoadStatus::solid);
    EXPECT_FALSE(empty->hasImage());
    EXPECT_EQ(empty->backgroundColor, QColor{"#182033"});

    const auto solid =
        loadAndWait(loader, request("unused.png", {}, 2U, config::WallpaperMode::solid));
    ASSERT_TRUE(solid);
    EXPECT_EQ(solid->status, WallpaperLoadStatus::solid);
}

TEST(WallpaperLoaderTest, DecodesGeneratedRasterFromAbsoluteAndExplicitRelativePaths) {
    TemporaryWallpapers files;
    QImage fixture{17, 11, QImage::Format_ARGB32_Premultiplied};
    fixture.fill(QColor{"#25a060"});
    const QString absolute = files.writeImage("wallpapers/generated.png", fixture);
    WallpaperLoader loader;

    const auto first = loadAndWait(loader, request(absolute, {}, 1U));
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->hasImage());
    EXPECT_EQ(first->status, WallpaperLoadStatus::ready);
    EXPECT_EQ(first->sourceSize, QSize(17, 11));
    EXPECT_EQ(first->image->pixelColor(8, 5), QColor{"#25a060"});

    const auto second = loadAndWait(loader, request("wallpapers/generated.png", files.root(), 2U));
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->hasImage());
    EXPECT_EQ(second->sourceSize, QSize(17, 11));
}

TEST(WallpaperLoaderTest, AppliesImageOrientationBeforePublishing) {
    TemporaryWallpapers files;
    QImage fixture{9, 5, QImage::Format_RGB32};
    fixture.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer{&bytes};
    ASSERT_TRUE(buffer.open(QIODevice::WriteOnly));
    QImageWriter writer{&buffer, "JPEG"};
    writer.setQuality(95);
    writer.setTransformation(QImageIOHandler::TransformationRotate90);
    if (!writer.write(fixture)) {
        GTEST_SKIP() << "The system JPEG plugin cannot write orientation metadata.";
    }
    const QString path = files.write("oriented.jpg", bytes);
    WallpaperLoader loader;

    const auto result = loadAndWait(loader, request(path, {}, 1U));
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->hasImage());
    EXPECT_EQ(result->sourceSize, QSize(5, 9));
}

TEST(WallpaperLoaderTest, RejectsRemoteUnsupportedAndNonRegularSources) {
    TemporaryWallpapers files;
    QDir{}.mkpath(files.path("directory"));
    const QString target = files.write("target.png", "not an image");
    const QString symlink = files.path("link.png");
    ASSERT_EQ(
        ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(symlink).constData()),
        0);
    const QString fifo = files.path("pipe");
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    WallpaperLoader loader;

    for (const QString &source :
         {QStringLiteral("https://example.invalid/wallpaper.png"),
          QStringLiteral("file:///tmp/wallpaper.png"), files.path("directory"), symlink, fifo}) {
        const auto result = loadAndWait(
            loader, request(source, {}, loader.current() ? loader.current()->generation + 1U : 1U));
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, WallpaperLoadStatus::unsupported);
        EXPECT_FALSE(result->hasImage());
    }
}

TEST(WallpaperLoaderTest, RejectsMissingMalformedOversizedAndAllocationBombSources) {
    TemporaryWallpapers files;
    const QString malformed = files.write("malformed.png", "not a supported image");
    const QString bomb = files.write("bomb.bmp", allocationBombBmp());
    const QString oversized = files.path("oversized.bin");
    QFile large{oversized};
    ASSERT_TRUE(large.open(QIODevice::WriteOnly));
    ASSERT_TRUE(large.resize(maximumWallpaperSourceBytes + 1));
    large.close();
    WallpaperLoader loader;

    const auto missing = loadAndWait(loader, request(files.path("missing.png"), {}, 1U));
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, WallpaperLoadStatus::unavailable);

    const auto invalid = loadAndWait(loader, request(malformed, {}, 2U));
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, WallpaperLoadStatus::unsupported);

    const auto tooLargeFile = loadAndWait(loader, request(oversized, {}, 3U));
    ASSERT_TRUE(tooLargeFile);
    EXPECT_EQ(tooLargeFile->status, WallpaperLoadStatus::tooLarge);

    const auto bombResult = loadAndWait(loader, request(bomb, {}, 4U));
    ASSERT_TRUE(bombResult);
    EXPECT_EQ(bombResult->status, WallpaperLoadStatus::tooLarge);
}

class BlockingDecoder final : public WallpaperDecoder {
  public:
    [[nodiscard]] WallpaperDecodeResult decode(const WallpaperRequest &request,
                                               const Cancelled &cancelled) const override {
        {
            const std::scoped_lock lock{mutex_};
            workerThread_ = QThread::currentThread();
        }
        if (request.generation == 1U) {
            {
                const std::scoped_lock lock{mutex_};
                firstEntered_ = true;
            }
            condition_.notify_all();
            while (!cancelled()) {
                std::unique_lock lock{mutex_};
                condition_.wait_for(lock, std::chrono::milliseconds{5});
            }
            return {WallpaperLoadStatus::invalid, {}};
        }
        auto image = std::make_shared<QImage>(3, 2, QImage::Format_ARGB32);
        image->fill(Qt::blue);
        return {WallpaperLoadStatus::ready, std::move(image)};
    }

    [[nodiscard]] bool waitForFirst() const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, std::chrono::seconds{2}, [this] { return firstEntered_; });
    }

    [[nodiscard]] QThread *workerThread() const {
        const std::scoped_lock lock{mutex_};
        return workerThread_;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable bool firstEntered_{false};
    mutable QThread *workerThread_{nullptr};
};

TEST(WallpaperLoaderTest, CoalescesPendingWorkAndRejectsStaleWorkerPublication) {
    auto decoder = std::make_shared<BlockingDecoder>();
    WallpaperLoader loader{decoder};
    QObject context;
    int staleCallbacks = 0;
    int currentCallbacks = 0;
    std::shared_ptr<const WallpaperSnapshot> published;
    ASSERT_TRUE(
        loader.load(request("first.png", {}, 1U), &context, [&](auto) { ++staleCallbacks; }));
    ASSERT_TRUE(decoder->waitForFirst());
    EXPECT_NE(decoder->workerThread(), context.thread());
    ASSERT_TRUE(loader.load(request("second.png", {}, 2U), &context, [&](auto snapshot) {
        ++currentCallbacks;
        published = std::move(snapshot);
    }));

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (published) {
            loop.quit();
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(3000);
    loop.exec();

    ASSERT_TRUE(published);
    EXPECT_EQ(staleCallbacks, 0);
    EXPECT_EQ(currentCallbacks, 1);
    EXPECT_EQ(published->generation, 2U);
    EXPECT_TRUE(published->hasImage());
    ASSERT_TRUE(loader.current());
    EXPECT_EQ(loader.current()->generation, 2U);
}

TEST(WallpaperLoaderTest, RejectsRepeatedAndZeroGenerationsWithRedactedErrors) {
    WallpaperLoader loader;
    QObject context;
    const auto zero = loader.load(request({}, {}, 0U), &context, [](auto) {});
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().message.find("/"), std::string::npos);
    ASSERT_TRUE(loader.load(request({}, {}, 1U), &context, [](auto) {}));
    const auto repeated = loader.load(request("private-name.png", {}, 1U), &context, [](auto) {});
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error().message.find("private-name"), std::string::npos);
    EXPECT_EQ(repeated.error().recovery.find("private-name"), std::string::npos);

    const auto invalidMode = loader.load(
        request({}, {}, 2U, static_cast<config::WallpaperMode>(255)), &context, [](auto) {});
    EXPECT_FALSE(invalidMode);

    QString invalidBase;
    invalidBase.append(QChar{0xd800});
    const auto invalidUnicode = loader.load(request({}, invalidBase, 3U), &context, [](auto) {});
    EXPECT_FALSE(invalidUnicode);
}

} // namespace
} // namespace prismdrake::desktop
