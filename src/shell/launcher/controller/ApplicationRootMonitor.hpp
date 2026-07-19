#pragma once

#include "ApplicationPaths.hpp"
#include "Result.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

namespace prismdrake::shell::launcher::controller {

inline constexpr std::size_t maximumApplicationMonitorWatches = 4096U;
inline constexpr std::size_t maximumApplicationMonitorRecoveryAttempts = 3U;

struct ApplicationRootMonitorOptions final {
    std::size_t maximumWatches{maximumApplicationMonitorWatches};
    std::chrono::milliseconds debounce{150};
    std::chrono::milliseconds recoveryDelay{250};
};

/// Event-driven, bounded watcher for XDG application roots and their desktop files.
///
/// Every observed mutation is coalesced into a complete catalog-refresh request. Root deletion
/// remains observable through its nearest existing ancestor. Capacity or kernel-watch exhaustion
/// is fail-soft: a bounded sequence of recovery attempts is scheduled, and explicit refresh asks
/// the monitor to try again without introducing a polling loop.
class ApplicationRootMonitor final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<ApplicationRootMonitor>>
    create(prismdrake::launcher::ApplicationPaths paths, ApplicationRootMonitorOptions options = {},
           QObject *parent = nullptr);

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
                           ApplicationRootMonitorOptions options, QObject *parent);

    void handleFilesystemEvent();
    void handleDebounce();
    void handleRecovery();
    void rebuildWatches(bool resetRecoveryBudget);
    void scheduleRecovery();
    void setDegraded(bool degraded);

    prismdrake::launcher::ApplicationPaths paths_;
    ApplicationRootMonitorOptions options_;
    QFileSystemWatcher watcher_;
    QTimer debounce_timer_;
    QTimer recovery_timer_;
    std::size_t recovery_attempts_{0U};
    bool degraded_{false};
};

} // namespace prismdrake::shell::launcher::controller
