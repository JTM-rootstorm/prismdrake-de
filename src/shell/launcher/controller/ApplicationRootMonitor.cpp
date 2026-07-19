#include "ApplicationRootMonitor.hpp"

#include <QDir>
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

[[nodiscard]] std::filesystem::path nearestExistingDirectory(std::filesystem::path path) {
    std::error_code error;
    while (!path.empty()) {
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
                  std::stop_token stopToken) {
    std::set<std::filesystem::path> unique;
    bool truncated = false;
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
        std::error_code statusError;
        if (!std::filesystem::is_directory(root, statusError) || statusError) {
            static_cast<void>(append(nearestExistingDirectory(root.parent_path())));
        } else {
            static_cast<void>(append(root));
        }
    }
    for (const auto &root : paths.applicationDirectories) {
        if (stopToken.stop_requested()) {
            return {{}, true};
        }
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
            const auto candidate = iterator->path();
            std::error_code typeError;
            const bool isDirectory = iterator->is_directory(typeError);
            if (typeError) {
                typeError.clear();
            } else if (isDirectory) {
                if (!append(candidate)) {
                    iterator.disable_recursion_pending();
                }
            } else if (iterator->is_regular_file(typeError) && !typeError &&
                       candidate.extension() == ".desktop") {
                static_cast<void>(append(candidate));
            }
            iterator.increment(iterationError);
        }
    }
    return {std::vector<std::filesystem::path>{unique.begin(), unique.end()}, truncated};
}

class DefaultApplicationWatchCandidateCollector final : public ApplicationWatchCandidateCollector {
  public:
    Result<ApplicationWatchCandidates> collect(const prismdrake::launcher::ApplicationPaths &paths,
                                               std::size_t limit,
                                               std::stop_token stopToken) override {
        return Result<ApplicationWatchCandidates>::success(
            collectCandidates(paths, limit, stopToken));
    }
};

} // namespace

class ApplicationRootMonitor::CollectionWorker final {
  public:
    CollectionWorker(ApplicationRootMonitor &owner, prismdrake::launcher::ApplicationPaths paths,
                     std::size_t limit,
                     std::unique_ptr<ApplicationWatchCandidateCollector> collector)
        : owner_(&owner), paths_(std::move(paths)), limit_(limit), collector_(std::move(collector)),
          thread_([this](std::stop_token stopToken) { run(stopToken); }) {}

    ~CollectionWorker() { stop(); }

    CollectionWorker(const CollectionWorker &) = delete;
    CollectionWorker &operator=(const CollectionWorker &) = delete;

    void submit(std::uint64_t generation) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            pending_generation_ = generation;
        }
        condition_.notify_one();
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            pending_generation_.reset();
        }
        thread_.request_stop();
        condition_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    void run(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this, &stopToken] {
                return stopping_ || stopToken.stop_requested() || pending_generation_.has_value();
            });
            if (stopping_ || stopToken.stop_requested()) {
                return;
            }
            const auto generation = *pending_generation_;
            pending_generation_.reset();
            lock.unlock();

            auto candidates = Result<ApplicationWatchCandidates>::failure(
                {ErrorCode::io_error, "Application watch discovery failed.",
                 "Retry bounded application-root watch discovery."});
            try {
                candidates = collector_->collect(paths_, limit_, stopToken);
            } catch (...) {
                // The owner keeps its last valid watches and enters bounded degraded recovery.
            }
            if (stopToken.stop_requested()) {
                return;
            }
            QPointer<ApplicationRootMonitor> guard{owner_};
            QMetaObject::invokeMethod(
                owner_,
                [guard, generation, candidates = std::move(candidates)]() mutable {
                    if (guard) {
                        guard->applyCollection(generation, std::move(candidates));
                    }
                },
                Qt::QueuedConnection);
        }
    }

    ApplicationRootMonitor *owner_;
    prismdrake::launcher::ApplicationPaths paths_;
    std::size_t limit_;
    std::unique_ptr<ApplicationWatchCandidateCollector> collector_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<std::uint64_t> pending_generation_;
    bool stopping_{false};
    std::jthread thread_;
};

Result<std::unique_ptr<ApplicationRootMonitor>>
ApplicationRootMonitor::create(prismdrake::launcher::ApplicationPaths paths,
                               ApplicationRootMonitorOptions options, QObject *parent,
                               std::unique_ptr<ApplicationWatchCandidateCollector> collector) {
    auto valid = validateOptions(paths, options);
    if (!valid) {
        return Result<std::unique_ptr<ApplicationRootMonitor>>::failure(valid.error());
    }
    if (!collector) {
        collector = std::make_unique<DefaultApplicationWatchCandidateCollector>();
    }
    auto monitor = std::unique_ptr<ApplicationRootMonitor>(
        new ApplicationRootMonitor(std::move(paths), options, parent, std::move(collector)));
    monitor->requestCollection(true);
    return Result<std::unique_ptr<ApplicationRootMonitor>>::success(std::move(monitor));
}

ApplicationRootMonitor::ApplicationRootMonitor(
    prismdrake::launcher::ApplicationPaths paths, ApplicationRootMonitorOptions options,
    QObject *parent, std::unique_ptr<ApplicationWatchCandidateCollector> collector)
    : QObject(parent), paths_(std::move(paths)), options_(options) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(options_.debounce);
    recovery_timer_.setSingleShot(true);
    recovery_timer_.setInterval(options_.recoveryDelay);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            &ApplicationRootMonitor::handleFilesystemEvent);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
            &ApplicationRootMonitor::handleFilesystemEvent);
    connect(&debounce_timer_, &QTimer::timeout, this, &ApplicationRootMonitor::handleDebounce);
    connect(&recovery_timer_, &QTimer::timeout, this, &ApplicationRootMonitor::handleRecovery);
    collection_worker_ = std::make_unique<CollectionWorker>(*this, paths_, options_.maximumWatches,
                                                            std::move(collector));
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
                                             Result<ApplicationWatchCandidates> candidates) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (generation != requested_generation_) {
        return;
    }
    if (!candidates) {
        setDegraded(true);
        scheduleRecovery();
        return;
    }
    const auto watched = watcher_.directories() + watcher_.files();
    if (!watched.empty()) {
        static_cast<void>(watcher_.removePaths(watched));
    }

    QStringList requested;
    requested.reserve(static_cast<qsizetype>(candidates.value().paths.size()));
    std::ranges::transform(
        candidates.value().paths, std::back_inserter(requested),
        [](const auto &path) { return QDir::cleanPath(QString::fromStdString(path)); });
    const auto rejected = requested.empty() ? QStringList{} : watcher_.addPaths(requested);
    setDegraded(candidates.value().truncated || !rejected.empty());
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
