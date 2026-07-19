#include "ApplicationRootMonitor.hpp"

#include <QDir>
#include <QStringList>

#include <algorithm>
#include <set>
#include <system_error>
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

struct WatchCandidates final {
    std::vector<std::filesystem::path> paths;
    bool truncated{false};
};

[[nodiscard]] WatchCandidates collectCandidates(const prismdrake::launcher::ApplicationPaths &paths,
                                                std::size_t limit) {
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
        std::error_code statusError;
        if (!std::filesystem::is_directory(root, statusError) || statusError) {
            static_cast<void>(append(nearestExistingDirectory(root.parent_path())));
            continue;
        }
        if (!append(root)) {
            continue;
        }
        std::error_code iterationError;
        std::filesystem::recursive_directory_iterator iterator{
            root, std::filesystem::directory_options::skip_permission_denied, iterationError};
        const std::filesystem::recursive_directory_iterator end;
        while (!iterationError && iterator != end) {
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

} // namespace

Result<std::unique_ptr<ApplicationRootMonitor>>
ApplicationRootMonitor::create(prismdrake::launcher::ApplicationPaths paths,
                               ApplicationRootMonitorOptions options, QObject *parent) {
    auto valid = validateOptions(paths, options);
    if (!valid) {
        return Result<std::unique_ptr<ApplicationRootMonitor>>::failure(valid.error());
    }
    auto monitor = std::unique_ptr<ApplicationRootMonitor>(
        new ApplicationRootMonitor(std::move(paths), options, parent));
    monitor->rebuildWatches(true);
    return Result<std::unique_ptr<ApplicationRootMonitor>>::success(std::move(monitor));
}

ApplicationRootMonitor::ApplicationRootMonitor(prismdrake::launcher::ApplicationPaths paths,
                                               ApplicationRootMonitorOptions options,
                                               QObject *parent)
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
}

void ApplicationRootMonitor::requestRecovery() { rebuildWatches(true); }

void ApplicationRootMonitor::handleFilesystemEvent() {
    debounce_timer_.start();
    recovery_timer_.stop();
}

void ApplicationRootMonitor::handleDebounce() {
    rebuildWatches(true);
    emit refreshRequested();
}

void ApplicationRootMonitor::handleRecovery() {
    ++recovery_attempts_;
    rebuildWatches(false);
}

void ApplicationRootMonitor::rebuildWatches(bool resetRecoveryBudget) {
    if (resetRecoveryBudget) {
        recovery_attempts_ = 0U;
        recovery_timer_.stop();
    }
    const auto watched = watcher_.directories() + watcher_.files();
    if (!watched.empty()) {
        static_cast<void>(watcher_.removePaths(watched));
    }

    auto candidates = collectCandidates(paths_, options_.maximumWatches);
    QStringList requested;
    requested.reserve(static_cast<qsizetype>(candidates.paths.size()));
    std::ranges::transform(candidates.paths, std::back_inserter(requested), [](const auto &path) {
        return QDir::cleanPath(QString::fromStdString(path));
    });
    const auto rejected = requested.empty() ? QStringList{} : watcher_.addPaths(requested);
    setDegraded(candidates.truncated || !rejected.empty());
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
