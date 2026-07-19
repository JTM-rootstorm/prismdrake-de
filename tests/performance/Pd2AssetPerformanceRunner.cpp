#include "IconResolver.hpp"
#include "WallpaperLoader.hpp"

#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::size_t minimumIterations = 3U;
constexpr std::size_t maximumIterations = 100U;
constexpr std::size_t defaultIterations = 7U;
constexpr int operationTimeoutMilliseconds = 5'000;

struct Options final {
    std::string sourceRevision;
    std::string environmentId;
    std::size_t iterations{defaultIterations};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/prismdrake-pd2-assets.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"temporary_directory_creation_failed"};
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[noreturn]] void fail(std::string_view identifier) {
    throw std::runtime_error{std::string{identifier}};
}

[[nodiscard]] bool validRevision(std::string_view value) {
    const bool length = value.size() == 40U || value.size() == 64U;
    return length && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool validEnvironmentId(std::string_view value) {
    if (value.empty() || value.size() > 64U ||
        !((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9'))) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == '-';
    });
}

[[nodiscard]] std::optional<std::size_t> parseCount(std::string_view value) {
    std::size_t parsed = 0U;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] Options parseOptions(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            fail("missing_option_value");
        }
        const std::string_view value{argv[++index]};
        if (argument == "--revision") {
            if (!options.sourceRevision.empty() || !validRevision(value)) {
                fail("invalid_source_revision");
            }
            options.sourceRevision = value;
        } else if (argument == "--environment-id") {
            if (!options.environmentId.empty() || !validEnvironmentId(value)) {
                fail("invalid_environment_id");
            }
            options.environmentId = value;
        } else if (argument == "--iterations") {
            const auto parsed = parseCount(value);
            if (!parsed || *parsed < minimumIterations || *parsed > maximumIterations) {
                fail("invalid_iteration_count");
            }
            options.iterations = *parsed;
        } else {
            fail("unknown_option");
        }
    }
    if (options.sourceRevision.empty() || options.environmentId.empty()) {
        fail("required_option_missing");
    }
    return options;
}

[[nodiscard]] std::uint64_t elapsed(Clock::time_point started, Clock::time_point finished) {
    const auto value = std::chrono::duration_cast<Nanoseconds>(finished - started).count();
    if (value < 0) {
        fail("non_monotonic_measurement");
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] prismdrake::icons::IconRequest iconRequest(const QString &path) {
    return {std::nullopt, path, 48, 1.0, 1U};
}

[[nodiscard]] std::uint64_t resolveIcon(prismdrake::icons::IconResolver &resolver,
                                        const QString &path) {
    QObject context;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    bool valid = false;
    Clock::time_point finished;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto started = Clock::now();
    auto queued = resolver.resolve(
        iconRequest(path), &context, [&](prismdrake::icons::IconResolution resolution) {
            finished = Clock::now();
            completed = true;
            valid = resolution.source == prismdrake::icons::IconResolutionSource::absolutePath &&
                    !resolution.image.isNull() && resolution.image.size() == QSize{48, 48} &&
                    QThread::currentThread() == context.thread();
            loop.quit();
        });
    if (!queued) {
        fail("icon_request_failed");
    }
    timeout.start(operationTimeoutMilliseconds);
    loop.exec();
    if (!completed || !valid || timeout.isActive() == false) {
        fail("icon_resolution_failed");
    }
    timeout.stop();
    return elapsed(started, finished);
}

[[nodiscard]] prismdrake::desktop::WallpaperRequest wallpaperRequest(const QString &path) {
    return {path, {}, QColor{"#182033"}, prismdrake::config::WallpaperMode::fill, 1U};
}

[[nodiscard]] std::uint64_t loadWallpaper(const QString &path) {
    prismdrake::desktop::WallpaperLoader loader;
    QObject context;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool completed = false;
    bool valid = false;
    Clock::time_point finished;
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto started = Clock::now();
    auto queued =
        loader.load(wallpaperRequest(path), &context,
                    [&](std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> snapshot) {
                        finished = Clock::now();
                        completed = true;
                        valid = snapshot && snapshot->hasImage() &&
                                snapshot->sourceSize == QSize{1600, 900} &&
                                QThread::currentThread() == context.thread();
                        loop.quit();
                    });
    if (!queued) {
        fail("wallpaper_request_failed");
    }
    timeout.start(operationTimeoutMilliseconds);
    loop.exec();
    if (!completed || !valid || timeout.isActive() == false) {
        fail("wallpaper_publication_failed");
    }
    timeout.stop();
    return elapsed(started, finished);
}

[[nodiscard]] nlohmann::json measurement(std::string_view name,
                                         std::vector<std::uint64_t> samples) {
    if (samples.size() < minimumIterations) {
        fail("measurement_series_invalid");
    }
    return {{"name", name}, {"samples_ns", std::move(samples)}};
}

[[nodiscard]] nlohmann::json collect(const Options &options) {
    TemporaryDirectory temporary;
    const auto iconPath = temporary.path() / "controlled-icon.png";
    QImage icon{128, 128, QImage::Format_ARGB32_Premultiplied};
    icon.fill(QColor{"#75d7ff"});
    if (!icon.save(QString::fromStdString(iconPath.string()), "PNG")) {
        fail("asset_fixture_creation_failed");
    }
    const auto wallpaperPath = temporary.path() / "controlled-wallpaper.png";
    QImage wallpaper{1600, 900, QImage::Format_ARGB32_Premultiplied};
    wallpaper.fill(QColor{"#182033"});
    if (!wallpaper.save(QString::fromStdString(wallpaperPath.string()), "PNG")) {
        fail("asset_fixture_creation_failed");
    }

    const QString iconSource = QString::fromStdString(iconPath.string());
    prismdrake::icons::IconResolver decodeWarmup;
    static_cast<void>(resolveIcon(decodeWarmup, iconSource));
    std::vector<std::uint64_t> decodeSamples;
    decodeSamples.reserve(options.iterations);
    for (std::size_t index = 0U; index < options.iterations; ++index) {
        prismdrake::icons::IconResolver resolver;
        decodeSamples.push_back(resolveIcon(resolver, iconSource));
    }

    prismdrake::icons::IconResolver cachedResolver;
    static_cast<void>(resolveIcon(cachedResolver, iconSource));
    std::vector<std::uint64_t> cacheSamples;
    cacheSamples.reserve(options.iterations);
    for (std::size_t index = 0U; index < options.iterations; ++index) {
        cacheSamples.push_back(resolveIcon(cachedResolver, iconSource));
    }

    const QString wallpaperSource = QString::fromStdString(wallpaperPath.string());
    std::vector<std::uint64_t> wallpaperSamples;
    wallpaperSamples.reserve(options.iterations);
    for (std::size_t index = 0U; index < options.iterations; ++index) {
        wallpaperSamples.push_back(loadWallpaper(wallpaperSource));
    }

    return {
        {"schema_version", 1},
        {"evidence_kind", "pd2_asset_performance"},
        {"source_revision", options.sourceRevision},
        {"reference_environment_id", options.environmentId},
        {"method",
         {{"clock", "std_chrono_steady_clock"},
          {"measured_iterations", options.iterations},
          {"icon_fixture", "warm_file_cache_controlled_local_png_128"},
          {"wallpaper_fixture", "warm_file_cache_controlled_local_png_1600x900"},
          {"completion", "production_async_callback_on_context_thread"}}},
        {"measurements",
         nlohmann::json::array(
             {measurement("icon_decode", std::move(decodeSamples)),
              measurement("icon_cache_hit", std::move(cacheSamples)),
              measurement("wallpaper_decode_publication", std::move(wallpaperSamples))})},
        {"redaction",
         {{"contains_filesystem_paths", false},
          {"contains_private_content", false},
          {"diagnostics_are_closed_ids", true}}},
    };
}

} // namespace

int main(int argc, char **argv) {
    try {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QGuiApplication application(argc, argv);
        const auto options = parseOptions(argc, argv);
        std::cout << collect(options).dump(2) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "prismdrake-pd2-asset-performance: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "prismdrake-pd2-asset-performance: unknown_bounded_failure\n";
        return 2;
    }
}
