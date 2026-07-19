#include "IconResolver.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::icons {
namespace {

class TemporaryIcons final {
  public:
    TemporaryIcons() {
        std::string pattern = "/tmp/prismdrake-icon-resolver-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create the icon fixture."};
        }
        root_ = created;
    }

    ~TemporaryIcons() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] QString path(const QString &relative) const {
        return QString::fromStdString((root_ / relative.toStdString()).string());
    }

    QString writeImage(const QString &relative, const QImage &image) const {
        const QString destination = path(relative);
        QDir{}.mkpath(QFileInfo{destination}.absolutePath());
        if (!image.save(destination)) {
            throw std::runtime_error{"Could not write the raster icon fixture."};
        }
        return destination;
    }

    QString writeText(const QString &relative, const QByteArray &contents) const {
        const QString destination = path(relative);
        QDir{}.mkpath(QFileInfo{destination}.absolutePath());
        QFile file(destination);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(contents) != contents.size()) {
            throw std::runtime_error{"Could not write the icon fixture."};
        }
        return destination;
    }

  private:
    std::filesystem::path root_;
};

struct CompletionResult final {
    IconResolution resolution;
    bool completed{false};
    bool callbackOnContextThread{false};
};

class ThemeStateGuard final {
  public:
    ThemeStateGuard() : paths_(QIcon::themeSearchPaths()), theme_(QIcon::themeName()) {}
    ~ThemeStateGuard() {
        QIcon::setThemeName(theme_);
        QIcon::setThemeSearchPaths(paths_);
    }

  private:
    QStringList paths_;
    QString theme_;
};

[[nodiscard]] CompletionResult resolveAndWait(IconResolver &resolver, IconRequest request) {
    QObject context;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    CompletionResult result;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto queued =
        resolver.resolve(std::move(request), &context, [&](IconResolution resolution) {
            result.resolution = std::move(resolution);
            result.completed = true;
            result.callbackOnContextThread = QThread::currentThread() == context.thread();
            loop.quit();
        });
    EXPECT_TRUE(queued);
    EXPECT_FALSE(result.completed);
    timeout.start(5000);
    loop.exec();
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(result.callbackOnContextThread);
    return result;
}

[[nodiscard]] IconRequest request(QString field, int size = 32, qreal dpr = 1.0,
                                  std::uint64_t themeGeneration = 1U) {
    return {std::nullopt, std::move(field), size, dpr, themeGeneration};
}

TEST(IconResolverTest, GuaranteedFallbackResolvesAtMultipleSizesAndDpr) {
    IconResolver resolver;
    for (const auto [size, dpr] : {std::pair{16, 1.0}, std::pair{24, 2.0}, std::pair{64, 1.5}}) {
        const auto result = resolveAndWait(resolver, request("missing-prismdrake-icon", size, dpr));
        EXPECT_EQ(result.resolution.source, IconResolutionSource::genericFallback);
        EXPECT_FALSE(result.resolution.image.isNull());
        EXPECT_EQ(result.resolution.image.width(), static_cast<int>(std::ceil(size * dpr)));
        EXPECT_EQ(result.resolution.image.height(), static_cast<int>(std::ceil(size * dpr)));
        EXPECT_DOUBLE_EQ(result.resolution.image.devicePixelRatio(), dpr);
    }
}

TEST(IconResolverTest, ResolvesValidatedRasterAndSvgAbsolutePaths) {
    TemporaryIcons icons;
    const QString raster = icons.writeImage("safe.png", QImage{48, 48, QImage::Format_ARGB32});
    constexpr auto svg = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10">
<rect width="10" height="10" fill="#25a060"/></svg>)";
    const QString vector = icons.writeText("safe.svg", svg);
    IconResolver resolver;

    const auto rasterResult = resolveAndWait(resolver, request(raster, 24, 2.0));
    EXPECT_EQ(rasterResult.resolution.source, IconResolutionSource::absolutePath);
    EXPECT_EQ(rasterResult.resolution.image.size(), QSize(48, 48));

    const auto vectorResult = resolveAndWait(resolver, request(vector, 40, 1.0));
    EXPECT_EQ(vectorResult.resolution.source, IconResolutionSource::absolutePath);
    EXPECT_EQ(vectorResult.resolution.image.size(), QSize(40, 40));
}

TEST(IconResolverTest, UsesSystemQtThemeLookup) {
    TemporaryIcons icons;
    ThemeStateGuard themeState;
    constexpr auto index = R"([Icon Theme]
Name=Prismdrake Test
Directories=16x16/apps..legacy,32x32/apps,32x32@2/apps,scalable/apps

[16x16/apps..legacy]
Size=16
Context=Applications
Type=Fixed

[32x32/apps]
Size=32
Context=Applications
Type=Fixed

[32x32@2/apps]
Size=32
Scale=2
Context=Applications
Type=Fixed

[scalable/apps]
Size=48
MinSize=16
MaxSize=64
Context=Applications
Type=Scalable
)";
    const QString themeIndex = icons.writeText("PrismdrakeTest/index.theme", index);
    const auto writeSolid = [&](const QString &directory, int size, const QColor &color) {
        QImage image{size, size, QImage::Format_ARGB32};
        image.fill(color);
        return icons.writeImage("PrismdrakeTest/" + directory + "/prismdrake-theme-fixture.png",
                                image);
    };
    const QString icon16 = writeSolid("16x16/apps..legacy", 16, Qt::red);
    const QString icon32 = writeSolid("32x32/apps", 32, Qt::green);
    const QString icon32x2 = writeSolid("32x32@2/apps", 64, Qt::yellow);
    const QString iconScalable = writeSolid("scalable/apps", 64, Qt::blue);
    EXPECT_FALSE(themeIndex.isEmpty());
    EXPECT_FALSE(icon16.isEmpty());
    EXPECT_FALSE(icon32.isEmpty());
    EXPECT_FALSE(icon32x2.isEmpty());
    EXPECT_FALSE(iconScalable.isEmpty());
    QIcon::setThemeSearchPaths({icons.path("")});
    QIcon::setThemeName("PrismdrakeTest");

    IconResolver resolver;
    const auto size16 = resolveAndWait(resolver, request("prismdrake-theme-fixture", 16, 1.0, 1U));
    const auto size32 = resolveAndWait(resolver, request("prismdrake-theme-fixture", 32, 1.0, 1U));
    const auto scalable =
        resolveAndWait(resolver, request("prismdrake-theme-fixture", 48, 1.0, 1U));
    const auto highDpi = resolveAndWait(resolver, request("prismdrake-theme-fixture", 32, 2.0, 1U));

    for (const auto *result : {&size16, &size32, &scalable, &highDpi}) {
        EXPECT_EQ(result->resolution.source, IconResolutionSource::theme);
        EXPECT_FALSE(result->resolution.image.isNull());
    }
    EXPECT_EQ(size16.resolution.image.pixelColor(8, 8), QColor{Qt::red});
    EXPECT_EQ(size32.resolution.image.pixelColor(16, 16), QColor{Qt::green});
    EXPECT_EQ(scalable.resolution.image.pixelColor(24, 24), QColor{Qt::blue});
    EXPECT_EQ(highDpi.resolution.image.pixelColor(32, 32), QColor{Qt::yellow});
}

TEST(IconResolverTest, CurrentThemeClosestPrecedesInheritedExact) {
    TemporaryIcons icons;
    ThemeStateGuard themeState;
    constexpr auto currentIndex = R"([Icon Theme]
Name=Current
Directories=16x16/apps
Inherits=Parent

[16x16/apps]
Size=16
Type=Fixed
)";
    constexpr auto parentIndex = R"([Icon Theme]
Name=Parent
Directories=32x32/apps

[32x32/apps]
Size=32
Type=Fixed
)";
    icons.writeText("Current/index.theme", currentIndex);
    icons.writeText("Parent/index.theme", parentIndex);
    QImage current{16, 16, QImage::Format_ARGB32};
    current.fill(Qt::green);
    QImage inherited{32, 32, QImage::Format_ARGB32};
    inherited.fill(Qt::red);
    icons.writeImage("Current/16x16/apps/prismdrake-inheritance-fixture.png", current);
    icons.writeImage("Parent/32x32/apps/prismdrake-inheritance-fixture.png", inherited);
    QIcon::setThemeSearchPaths({icons.path("")});
    QIcon::setThemeName("Current");
    IconResolver resolver;

    const auto result =
        resolveAndWait(resolver, request("prismdrake-inheritance-fixture", 32, 1.0));

    EXPECT_EQ(result.resolution.source, IconResolutionSource::theme);
    EXPECT_EQ(result.resolution.image.pixelColor(16, 16), QColor{Qt::green});
}

TEST(IconResolverTest, EarlierSearchRootPrecedesLaterExactAsset) {
    TemporaryIcons firstRoot;
    TemporaryIcons secondRoot;
    ThemeStateGuard themeState;
    constexpr auto smallIndex = R"([Icon Theme]
Name=RootOrder
Directories=16x16/apps

[16x16/apps]
Size=16
Type=Fixed
)";
    constexpr auto exactIndex = R"([Icon Theme]
Name=RootOrder
Directories=32x32/apps

[32x32/apps]
Size=32
Type=Fixed
)";
    firstRoot.writeText("RootOrder/index.theme", smallIndex);
    secondRoot.writeText("RootOrder/index.theme", exactIndex);
    QImage first{16, 16, QImage::Format_ARGB32};
    first.fill(Qt::red);
    QImage second{32, 32, QImage::Format_ARGB32};
    second.fill(Qt::blue);
    firstRoot.writeImage("RootOrder/16x16/apps/prismdrake-root-order-fixture.png", first);
    secondRoot.writeImage("RootOrder/32x32/apps/prismdrake-root-order-fixture.png", second);
    QIcon::setThemeSearchPaths({firstRoot.path(""), secondRoot.path("")});
    QIcon::setThemeName("RootOrder");
    IconResolver resolver;

    const auto result = resolveAndWait(resolver, request("prismdrake-root-order-fixture", 32, 1.0));

    EXPECT_EQ(result.resolution.source, IconResolutionSource::theme);
    EXPECT_EQ(result.resolution.image.pixelColor(16, 16), QColor{Qt::red});
}

TEST(IconResolverTest, ExcessiveThemeSearchRootsFailSoftToGenericFallback) {
    TemporaryIcons icons;
    ThemeStateGuard themeState;
    constexpr auto index = R"([Icon Theme]
Name=BoundedRoots
Directories=32x32/apps

[32x32/apps]
Size=32
Type=Fixed
)";
    icons.writeText("BoundedRoots/index.theme", index);
    QImage image{32, 32, QImage::Format_ARGB32};
    image.fill(Qt::green);
    icons.writeImage("BoundedRoots/32x32/apps/prismdrake-root-bound-fixture.png", image);
    QStringList roots{icons.path("")};
    while (static_cast<std::size_t>(roots.size()) <= maximumIconThemeSearchRoots) {
        roots.push_back(QStringLiteral("/tmp/prismdrake-unused-icon-root-%1").arg(roots.size()));
    }
    QIcon::setThemeSearchPaths(roots);
    QIcon::setThemeName("BoundedRoots");
    IconResolver resolver;

    const auto result = resolveAndWait(resolver, request("prismdrake-root-bound-fixture"));

    EXPECT_EQ(result.resolution.source, IconResolutionSource::genericFallback);
    EXPECT_FALSE(result.resolution.image.isNull());
}

TEST(IconResolverTest, ExcessiveThemeSearchPathBytesFailSoftToGenericFallback) {
    TemporaryIcons icons;
    ThemeStateGuard themeState;
    constexpr auto index = R"([Icon Theme]
Name=BoundedBytes
Directories=32x32/apps

[32x32/apps]
Size=32
Type=Fixed
)";
    icons.writeText("BoundedBytes/index.theme", index);
    QImage image{32, 32, QImage::Format_ARGB32};
    image.fill(Qt::green);
    icons.writeImage("BoundedBytes/32x32/apps/prismdrake-byte-bound-fixture.png", image);
    const QString oversizedRoot =
        QStringLiteral("/") + QString(maximumIconThemeSearchPathBytes, QLatin1Char('a'));
    QIcon::setThemeSearchPaths({icons.path(""), oversizedRoot});
    QIcon::setThemeName("BoundedBytes");
    IconResolver resolver;

    const auto result = resolveAndWait(resolver, request("prismdrake-byte-bound-fixture"));

    EXPECT_EQ(result.resolution.source, IconResolutionSource::genericFallback);
    EXPECT_FALSE(result.resolution.image.isNull());
}

TEST(IconResolverTest, RejectsLeafSymlinkAndFifoWithoutBlockingWorker) {
    TemporaryIcons icons;
    QImage safe{16, 16, QImage::Format_ARGB32};
    safe.fill(Qt::green);
    const QString target = icons.writeImage("target.png", safe);
    const QString link = icons.path("link.png");
    std::error_code linkError;
    std::filesystem::create_symlink(target.toStdString(), link.toStdString(), linkError);
    ASSERT_FALSE(linkError);
    const QString fifo = icons.path("source.fifo");
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600U), 0);
    IconResolver resolver;

    for (const QString &field : {link, fifo}) {
        const auto result = resolveAndWait(resolver, request(field));
        EXPECT_EQ(result.resolution.source, IconResolutionSource::genericFallback);
        EXPECT_FALSE(result.resolution.image.isNull());
    }
}

TEST(IconResolverTest, RejectsWorkPastBoundedOutstandingQueue) {
    TemporaryIcons icons;
    QByteArray slow = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><!--)";
    slow.append(3 * 1024 * 1024, 'x');
    slow.append(R"(--><rect width="10" height="10" fill="#3080c0"/></svg>)");
    const QString source = icons.writeText("slow.svg", slow);
    IconResolver resolver{1U};
    QObject context;
    std::size_t completed = 0U;
    ASSERT_TRUE(resolver.resolve(request(source, 32, 1.0, 1U), &context,
                                 [&](IconResolution) { ++completed; }));

    bool sawQueueLimit = false;
    for (std::uint64_t generation = 2U; generation < 128U && !sawQueueLimit; ++generation) {
        const auto queued = resolver.resolve(request(source, 32, 1.0, generation), &context,
                                             [&](IconResolution) { ++completed; });
        if (!queued) {
            EXPECT_EQ(queued.error().code, foundation::ErrorCode::too_large);
            sawQueueLimit = true;
        }
    }
    EXPECT_TRUE(sawQueueLimit);
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    EXPECT_GE(completed, 1U);
}

TEST(IconResolverTest, RejectsUnsafeOversizedAnimatedOrMalformedSourcesToFallback) {
    TemporaryIcons icons;
    const QString oversizedBytes =
        icons.writeText("oversized.png", QByteArray(maximumIconSourceBytes + 1, 'x'));
    QImage oversizedDimensions{maximumIconSourceDimension + 1, 1, QImage::Format_ARGB32};
    oversizedDimensions.fill(Qt::red);
    const QString oversizedImage = icons.writeImage("wide.png", oversizedDimensions);
    const QString malformed = icons.writeText("malformed.svg", "<svg><invalid>");
    const QByteArray animatedGif = QByteArray::fromHex(
        "47494638396101000100800000000000ffffff21f90401000000002c000000000100010000"
        "020244010021f90401000000002c00000000010001000002024401003b");
    const QString animated = icons.writeText("animated.gif", animatedGif);
    const QString relative = QStringLiteral("relative/path.png");
    IconResolver resolver;

    for (const QString &field : {oversizedBytes, oversizedImage, malformed, animated, relative,
                                 QStringLiteral("https://invalid/icon.png")}) {
        const auto result = resolveAndWait(resolver, request(field));
        EXPECT_EQ(result.resolution.source, IconResolutionSource::genericFallback);
        EXPECT_FALSE(result.resolution.image.isNull());
    }
}

TEST(IconResolverTest, CacheKeyIncludesThemeGenerationAndQueuesCompletion) {
    TemporaryIcons icons;
    QImage first{16, 16, QImage::Format_ARGB32};
    first.fill(Qt::red);
    const QString path = icons.writeImage("mutable.png", first);
    IconResolver resolver;

    const auto generationOne = resolveAndWait(resolver, request(path, 16, 1.0, 10U));
    QImage second{16, 16, QImage::Format_ARGB32};
    second.fill(Qt::blue);
    EXPECT_EQ(icons.writeImage("mutable.png", second), path);
    const auto cached = resolveAndWait(resolver, request(path, 16, 1.0, 10U));
    const auto refreshed = resolveAndWait(resolver, request(path, 16, 1.0, 11U));

    EXPECT_EQ(generationOne.resolution.image.pixelColor(8, 8), QColor{Qt::red});
    EXPECT_EQ(cached.resolution.image.pixelColor(8, 8), QColor{Qt::red});
    EXPECT_EQ(refreshed.resolution.image.pixelColor(8, 8), QColor{Qt::blue});
}

TEST(IconResolverTest, RejectsInvalidImmutableRequestsSynchronously) {
    IconResolver resolver;
    QObject context;
    const auto completion = [](IconResolution) {};
    EXPECT_FALSE(
        resolver.resolve(request("safe", minimumIconLogicalSize - 1), &context, completion));
    EXPECT_FALSE(
        resolver.resolve(request("safe", maximumIconLogicalSize + 1), &context, completion));
    EXPECT_FALSE(resolver.resolve(request("safe", 32, 0.0), &context, completion));
    EXPECT_FALSE(resolver.resolve(request("safe", 32, maximumIconDevicePixelRatio + 0.1), &context,
                                  completion));
    EXPECT_FALSE(resolver.resolve(request("safe", 32, 1.0, 0U), &context, completion));
    EXPECT_FALSE(resolver.resolve(request("safe"), nullptr, completion));
    EXPECT_FALSE(resolver.resolve(request("safe"), &context, {}));

    QThread foreignThread;
    foreignThread.start();
    auto *foreignContext = new QObject;
    foreignContext->moveToThread(&foreignThread);
    EXPECT_FALSE(resolver.resolve(request("safe"), foreignContext, completion));
    QMetaObject::invokeMethod(
        foreignContext, [foreignContext] { delete foreignContext; }, Qt::BlockingQueuedConnection);
    foreignThread.quit();
    foreignThread.wait();
}

} // namespace
} // namespace prismdrake::icons
