#pragma once

#include "ApplicationPaths.hpp"
#include "Result.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

namespace prismdrake::shell::launcher::controller {

inline constexpr std::size_t maximumApplicationMonitorWatches = 4096U;
inline constexpr std::size_t maximumApplicationMonitorRecoveryAttempts = 3U;

struct ApplicationRootMonitorOptions final {
    std::size_t maximumWatches{maximumApplicationMonitorWatches};
    std::chrono::milliseconds debounce{150};
    std::chrono::milliseconds recoveryDelay{250};
};

struct ApplicationWatchCandidates final {
    std::vector<std::filesystem::path> paths;
    bool truncated{false};
};

/// Injectable blocking filesystem boundary used only by the monitor's dedicated worker.
class ApplicationWatchCandidateCollector {
  public:
    virtual ~ApplicationWatchCandidateCollector() = default;

    [[nodiscard]] virtual foundation::Result<ApplicationWatchCandidates>
    collect(const prismdrake::launcher::ApplicationPaths &paths, std::size_t limit,
            std::stop_token stopToken) = 0;
};

/// Event-driven, bounded watcher for XDG application roots and their desktop files.
///
/// Every observed mutation is coalesced into a complete catalog-refresh request. Root deletion
/// remains observable through its nearest existing ancestor. Capacity or kernel-watch exhaustion
/// is fail-soft: a bounded sequence of recovery attempts is scheduled, and explicit refresh asks
/// the monitor to try again without introducing a polling loop. Recursive discovery runs on one
/// dedicated worker; QFileSystemWatcher state is mutated only on this QObject's owner thread.
class ApplicationRootMonitor final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<ApplicationRootMonitor>>
    create(prismdrake::launcher::ApplicationPaths paths, ApplicationRootMonitorOptions options = {},
           QObject *parent = nullptr,
           std::unique_ptr<ApplicationWatchCandidateCollector> collector = {});

    ~ApplicationRootMonitor() override;

    [[nodiscard]] bool degraded() const noexcept { return degraded_; }
    [[nodiscard]] qsizetype watchedPathCount() const noexcept {
        return watcher_.directories().size() + watcher_.files().size();
    }

    /// Re-arms immediately after an explicit user recovery action.
    void requestRecovery();

  signals:
    void refreshRequested();
    void degradedChanged(bool degraded);

  private:
    ApplicationRootMonitor(prismdrake::launcher::ApplicationPaths paths,
                           ApplicationRootMonitorOptions options, QObject *parent,
                           std::unique_ptr<ApplicationWatchCandidateCollector> collector);

    void handleFilesystemEvent();
    void handleDebounce();
    void handleRecovery();
    void requestCollection(bool resetRecoveryBudget);
    void applyCollection(std::uint64_t generation,
                         foundation::Result<ApplicationWatchCandidates> candidates);
    void scheduleRecovery();
    void setDegraded(bool degraded);

    class CollectionWorker;

    prismdrake::launcher::ApplicationPaths paths_;
    ApplicationRootMonitorOptions options_;
    QFileSystemWatcher watcher_;
    QTimer debounce_timer_;
    QTimer recovery_timer_;
    std::unique_ptr<CollectionWorker> collection_worker_;
    std::size_t recovery_attempts_{0U};
    std::uint64_t requested_generation_{0U};
    bool degraded_{false};
};

} // namespace prismdrake::shell::launcher::controller
