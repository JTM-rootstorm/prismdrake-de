#include "ApplicationRootMonitor.hpp"

#include <QDir>
#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

namespace prismdrake::shell::launcher::controller {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<void> validateOptions(const prismdrake::launcher::ApplicationPaths &paths,
                                           const ApplicationRootMonitorOptions &options) {
    if (paths.applicationDirectories.empty() ||
        paths.applicationDirectories.size() >
            prismdrake::launcher::maximumApplicationDataDirectoryEntries ||
        options.maximumWatches == 0U || options.maximumWatches > maximumApplicationMonitorWatches ||
        options.maximumDiscoveryWork < paths.applicationDirectories.size() ||
        options.maximumDiscoveryWork > maximumApplicationMonitorDiscoveryWork ||
        options.debounce.count() < 0 || options.debounce > std::chrono::seconds{5} ||
        options.recoveryDelay.count() < 0 || options.recoveryDelay > std::chrono::seconds{30}) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The application-root monitor options are invalid.",
             "Use bounded absolute roots, watch capacity, debounce, and recovery delays."});
    }
    for (const auto &root : paths.applicationDirectories) {
        if (!root.is_absolute() || root.filename().empty()) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "An application-root monitor path is invalid.",
                                          "Use normalized absolute XDG application roots."});
        }
    }
    return Result<void>::success();
}

[[nodiscard]] std::filesystem::path
nearestExistingDirectory(std::filesystem::path path, std::size_t &remainingWork, bool &truncated) {
    std::error_code error;
    while (!path.empty()) {
        if (remainingWork == 0U) {
            truncated = true;
            return {};
        }
        --remainingWork;
        if (std::filesystem::is_directory(path, error) && !error) {
            return path;
        }
        error.clear();
        const auto parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return {};
}

[[nodiscard]] ApplicationWatchCandidates
collectCandidates(const prismdrake::launcher::ApplicationPaths &paths, std::size_t limit,
                  std::size_t workLimit, std::stop_token stopToken) {
    std::set<std::filesystem::path> unique;
    bool truncated = false;
    std::size_t remainingWork = workLimit;
    const auto append = [&](const std::filesystem::path &candidate) {
        if (candidate.empty() || unique.contains(candidate)) {
            return true;
        }
        if (unique.size() >= limit) {
            truncated = true;
            return false;
        }
        unique.emplace(candidate);
        return true;
    };

    for (const auto &root : paths.applicationDirectories) {
        if (stopToken.stop_requested()) {
            return {{}, true};
        }
        if (remainingWork == 0U) {
            return {std::vector<std::filesystem::path>(unique.begin(), unique.end()), true};
        }
        --remainingWork;
        std::error_code statusError;
        if (!std::filesystem::is_directory(root, statusError) || statusError) {
            static_cast<void>(
                append(nearestExistingDirectory(root.parent_path(), remainingWork, truncated)));
        } else {
            static_cast<void>(append(root));
        }
    }
    for (const auto &root : paths.applicationDirectories) {
        if (stopToken.stop_requested()) {
            return {{}, true};
        }
        if (remainingWork == 0U) {
            return {std::vector<std::filesystem::path>(unique.begin(), unique.end()), true};
        }
        --remainingWork;
        std::error_code statusError;
        if (!std::filesystem::is_directory(root, statusError) || statusError) {
            continue;
        }
        std::error_code iterationError;
        std::filesystem::recursive_directory_iterator iterator{
            root, std::filesystem::directory_options::skip_permission_denied, iterationError};
        const std::filesystem::recursive_directory_iterator end;
        while (!iterationError && iterator != end) {
            if (stopToken.stop_requested()) {
                return {{}, true};
            }
            if (remainingWork == 0U) {
                return {std::vector<std::filesystem::path>(unique.begin(), unique.end()), true};
            }
            --remainingWork;
            const auto candidate = iterator->path();
            std::error_code typeError;
            const bool isDirectory = iterator->is_directory(typeError);
            if (typeError) {
                typeError.clear();
            } else if (isDirectory) {
                if (!append(candidate)) {
                    return {std::vector<std::filesystem::path>(unique.begin(), unique.end()), true};
                }
            } else if (iterator->is_regular_file(typeError) && !typeError &&
                       candidate.extension() == ".desktop") {
                if (!append(candidate)) {
                    return {std::vector<std::filesystem::path>(unique.begin(), unique.end()), true};
                }
            }
            iterator.increment(iterationError);
        }
    }
    return {std::vector<std::filesystem::path>{unique.begin(), unique.end()}, truncated};
}

class DefaultApplicationWatchCandidateCollector final : public ApplicationWatchCandidateCollector {
  public:
    Result<ApplicationWatchCandidates> collect(const prismdrake::launcher::ApplicationPaths &paths,
                                               std::size_t limit, std::size_t workLimit,
                                               std::stop_token stopToken) override {
        return Result<ApplicationWatchCandidates>::success(
            collectCandidates(paths, limit, workLimit, stopToken));
    }
};

class QtApplicationPathWatcher final : public ApplicationPathWatcher {
  public:
    QtApplicationPathWatcher() {
        QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, &watcher_,
                         [this] { notifyChange(); });
        QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, &watcher_,
                         [this] { notifyChange(); });
    }

    void setChangeCallback(ChangeCallback callback) override { callback_ = std::move(callback); }

    Result<ApplicationWatchRegistration> replace(const std::vector<std::filesystem::path> &paths,
                                                 std::stop_token stopToken) override {
        constexpr qsizetype batchSize = 64;
        const auto removeInBatches = [&](QStringList current) {
            while (!current.empty() && !stopToken.stop_requested()) {
                const QStringList batch = current.mid(0, batchSize);
                current.erase(current.begin(), current.begin() + batch.size());
                static_cast<void>(watcher_.removePaths(batch));
            }
        };
        removeInBatches(watcher_.directories());
        removeInBatches(watcher_.files());
        if (stopToken.stop_requested()) {
            return Result<ApplicationWatchRegistration>::failure(
                {ErrorCode::cancelled, "Application watch registration was cancelled.",
                 "Destroy the stopping application-root monitor."});
        }

        QStringList requested;
        requested.reserve(static_cast<qsizetype>(paths.size()));
        std::ranges::transform(paths, std::back_inserter(requested), [](const auto &path) {
            return QDir::cleanPath(QString::fromStdString(path));
        });
        bool rejected = false;
        for (qsizetype offset = 0; offset < requested.size(); offset += batchSize) {
            if (stopToken.stop_requested()) {
                return Result<ApplicationWatchRegistration>::failure(
                    {ErrorCode::cancelled, "Application watch registration was cancelled.",
                     "Destroy the stopping application-root monitor."});
            }
            rejected = !watcher_.addPaths(requested.mid(offset, batchSize)).empty() || rejected;
        }
        return Result<ApplicationWatchRegistration>::success(
            {static_cast<std::size_t>(watcher_.directories().size() + watcher_.files().size()),
             rejected});
    }

  private:
    void notifyChange() {
        if (callback_) {
            callback_();
        }
    }

    QFileSystemWatcher watcher_;
    ChangeCallback callback_;
};

} // namespace

class ApplicationRootMonitor::CollectionWorker final {
  public:
    CollectionWorker(ApplicationRootMonitor &owner, prismdrake::launcher::ApplicationPaths paths,
                     std::size_t limit, std::size_t workLimit,
                     std::unique_ptr<ApplicationWatchCandidateCollector> collector,
                     ApplicationPathWatcherFactory watcherFactory)
        : owner_(&owner), paths_(std::move(paths)), limit_(limit), work_limit_(workLimit),
          collector_(std::move(collector)), watcher_factory_(std::move(watcherFactory)) {
        context_ = new QObject;
        context_->moveToThread(&thread_);
        QObject::connect(&thread_, &QThread::finished, context_, &QObject::deleteLater);
        thread_.start();
    }

    ~CollectionWorker() { stop(); }

    CollectionWorker(const CollectionWorker &) = delete;
    CollectionWorker &operator=(const CollectionWorker &) = delete;

    void submit(std::uint64_t generation) {
        if (stopping_.load(std::memory_order_acquire)) {
            return;
        }
        latest_generation_.store(generation, std::memory_order_release);
        static_cast<void>(QMetaObject::invokeMethod(
            context_, [this, generation] { process(generation); }, Qt::QueuedConnection));
    }

    void stop() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        stop_source_.request_stop();
        if (thread_.isRunning()) {
            static_cast<void>(QMetaObject::invokeMethod(
                context_, [this] { watcher_.reset(); }, Qt::BlockingQueuedConnection));
            thread_.quit();
            static_cast<void>(thread_.wait());
        }
    }

  private:
    void process(std::uint64_t generation) {
        Q_ASSERT(QThread::currentThread() == context_->thread());
        const auto stopToken = stop_source_.get_token();
        if (stopping_.load(std::memory_order_acquire) || stopToken.stop_requested() ||
            generation != latest_generation_.load(std::memory_order_acquire)) {
            return;
        }

        auto candidates = Result<ApplicationWatchCandidates>::failure(
            {ErrorCode::io_error, "Application watch discovery failed.",
             "Retry bounded application-root watch discovery."});
        try {
            candidates = collector_->collect(paths_, limit_, work_limit_, stopToken);
        } catch (...) {
            // The owner keeps its last valid watches and enters bounded degraded recovery.
        }
        if (stopping_.load(std::memory_order_acquire) || stopToken.stop_requested() ||
            generation != latest_generation_.load(std::memory_order_acquire)) {
            return;
        }

        Result<ApplicationWatchRegistration> registration =
            Result<ApplicationWatchRegistration>::failure(
                {ErrorCode::io_error, "Application watch registration failed.",
                 "Retry bounded application-root watch registration."});
        bool truncated = false;
        if (candidates) {
            truncated = candidates.value().truncated;
            if (!watcher_) {
                try {
                    watcher_ = watcher_factory_();
                } catch (...) {
                    watcher_.reset();
                }
                if (watcher_) {
                    watcher_->setChangeCallback([this] { notifyOwnerOfChange(); });
                }
            }
            if (watcher_) {
                try {
                    registration = watcher_->replace(candidates.value().paths, stopToken);
                } catch (...) {
                    // Registration failures retain the bounded degraded-recovery path.
                }
            }
        }
        if (stopping_.load(std::memory_order_acquire) || stopToken.stop_requested()) {
            return;
        }
        if (generation != latest_generation_.load(std::memory_order_acquire)) {
            watcher_.reset();
            owner_->watched_path_count_.store(0, std::memory_order_release);
            return;
        }
        publish(generation, std::move(registration), truncated);
    }

    void notifyOwnerOfChange() {
        QPointer<ApplicationRootMonitor> guard{owner_};
        static_cast<void>(QMetaObject::invokeMethod(
            owner_,
            [guard] {
                if (guard) {
                    guard->handleFilesystemEvent();
                }
            },
            Qt::QueuedConnection));
    }

    void publish(std::uint64_t generation, Result<ApplicationWatchRegistration> registration,
                 bool truncated) {
        QPointer<ApplicationRootMonitor> guard{owner_};
        static_cast<void>(QMetaObject::invokeMethod(
            owner_,
            [guard, generation, registration = std::move(registration), truncated]() mutable {
                if (guard) {
                    guard->applyCollection(generation, std::move(registration), truncated);
                }
            },
            Qt::QueuedConnection));
    }

    ApplicationRootMonitor *owner_;
    prismdrake::launcher::ApplicationPaths paths_;
    std::size_t limit_;
    std::size_t work_limit_;
    std::unique_ptr<ApplicationWatchCandidateCollector> collector_;
    ApplicationPathWatcherFactory watcher_factory_;
    std::unique_ptr<ApplicationPathWatcher> watcher_;
    std::stop_source stop_source_;
    std::atomic<std::uint64_t> latest_generation_{0U};
    std::atomic_bool stopping_{false};
    QThread thread_;
    QObject *context_{nullptr};
};

Result<std::unique_ptr<ApplicationRootMonitor>>
ApplicationRootMonitor::create(prismdrake::launcher::ApplicationPaths paths,
                               ApplicationRootMonitorOptions options, QObject *parent,
                               std::unique_ptr<ApplicationWatchCandidateCollector> collector,
                               ApplicationPathWatcherFactory watcherFactory) {
    auto valid = validateOptions(paths, options);
    if (!valid) {
        return Result<std::unique_ptr<ApplicationRootMonitor>>::failure(valid.error());
    }
    if (!collector) {
        collector = std::make_unique<DefaultApplicationWatchCandidateCollector>();
    }
    if (!watcherFactory) {
        watcherFactory = [] { return std::make_unique<QtApplicationPathWatcher>(); };
    }
    auto monitor = std::unique_ptr<ApplicationRootMonitor>(new ApplicationRootMonitor(
        std::move(paths), options, parent, std::move(collector), std::move(watcherFactory)));
    monitor->requestCollection(true);
    return Result<std::unique_ptr<ApplicationRootMonitor>>::success(std::move(monitor));
}

ApplicationRootMonitor::ApplicationRootMonitor(
    prismdrake::launcher::ApplicationPaths paths, ApplicationRootMonitorOptions options,
    QObject *parent, std::unique_ptr<ApplicationWatchCandidateCollector> collector,
    ApplicationPathWatcherFactory watcherFactory)
    : QObject(parent), paths_(std::move(paths)), options_(options) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(options_.debounce);
    recovery_timer_.setSingleShot(true);
    recovery_timer_.setInterval(options_.recoveryDelay);
    connect(&debounce_timer_, &QTimer::timeout, this, &ApplicationRootMonitor::handleDebounce);
    connect(&recovery_timer_, &QTimer::timeout, this, &ApplicationRootMonitor::handleRecovery);
    collection_worker_ = std::make_unique<CollectionWorker>(
        *this, paths_, options_.maximumWatches, options_.maximumDiscoveryWork, std::move(collector),
        std::move(watcherFactory));
}

ApplicationRootMonitor::~ApplicationRootMonitor() { collection_worker_.reset(); }

void ApplicationRootMonitor::requestRecovery() { requestCollection(true); }

void ApplicationRootMonitor::handleFilesystemEvent() {
    debounce_timer_.start();
    recovery_timer_.stop();
}

void ApplicationRootMonitor::handleDebounce() {
    requestCollection(true);
    emit refreshRequested();
}

void ApplicationRootMonitor::handleRecovery() {
    ++recovery_attempts_;
    requestCollection(false);
}

void ApplicationRootMonitor::requestCollection(bool resetRecoveryBudget) {
    if (resetRecoveryBudget) {
        recovery_attempts_ = 0U;
        recovery_timer_.stop();
    }
    if (requested_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        setDegraded(true);
        return;
    }
    ++requested_generation_;
    collection_worker_->submit(requested_generation_);
}

void ApplicationRootMonitor::applyCollection(std::uint64_t generation,
                                             Result<ApplicationWatchRegistration> registration,
                                             bool truncated) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != requested_generation_) {
        return;
    }
    if (!registration) {
        setDegraded(true);
        scheduleRecovery();
        return;
    }
    watched_path_count_.store(static_cast<qsizetype>(registration.value().installed),
                              std::memory_order_release);
    setDegraded(truncated || registration.value().rejected);
    if (degraded_) {
        scheduleRecovery();
    }
}

void ApplicationRootMonitor::scheduleRecovery() {
    if (recovery_attempts_ >= maximumApplicationMonitorRecoveryAttempts ||
        recovery_timer_.isActive()) {
        return;
    }
    const auto multiplier = 1 << std::min<std::size_t>(recovery_attempts_, 6U);
    recovery_timer_.setInterval(options_.recoveryDelay * multiplier);
    recovery_timer_.start();
}

void ApplicationRootMonitor::setDegraded(bool degraded) {
    if (degraded_ == degraded) {
        return;
    }
    degraded_ = degraded;
    emit degradedChanged(degraded_);
}

} // namespace prismdrake::shell::launcher::controller
