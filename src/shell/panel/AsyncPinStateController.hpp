#pragma once

#include "PinState.hpp"

#include <QObject>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace prismdrake::shell::panel {

/// Blocking persistence boundary used exclusively by AsyncPinStateController's worker thread.
class PinStatePersistenceBackend {
  public:
    virtual ~PinStatePersistenceBackend() = default;

    /// Interrupts an injected blocking backend before worker teardown. Implementations must make
    /// this method safe to call concurrently with open() or replace().
    virtual void requestStop() noexcept {}

    [[nodiscard]] virtual foundation::Result<std::shared_ptr<const PinStateSnapshot>>
    open(const std::filesystem::path &prismdrakeStateDirectory) = 0;
    [[nodiscard]] virtual foundation::Result<std::shared_ptr<const PinStateSnapshot>>
    replace(std::vector<launcher::DesktopFileId> pins) = 0;
};

/// Owner-thread pin snapshot coordinator with one bounded dedicated persistence worker.
///
/// Initial loading and complete-snapshot replacement are serialized. At most one operation may be
/// outstanding; another request fails closed instead of creating an unbounded queue. The current
/// snapshot changes only on the QObject owner thread after successful durable persistence.
class AsyncPinStateController final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<AsyncPinStateController>>
    create(std::filesystem::path prismdrakeStateDirectory, QObject *parent = nullptr);

    /// Injectable construction boundary for deterministic thread and failure tests.
    [[nodiscard]] static foundation::Result<std::unique_ptr<AsyncPinStateController>>
    create(std::filesystem::path prismdrakeStateDirectory,
           std::unique_ptr<PinStatePersistenceBackend> backend, QObject *parent = nullptr);

    ~AsyncPinStateController() override;
    AsyncPinStateController(const AsyncPinStateController &) = delete;
    AsyncPinStateController &operator=(const AsyncPinStateController &) = delete;

    /// Starts one asynchronous initial load. No filesystem work occurs on the caller thread.
    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    /// Queues one complete replacement when no load or replacement is outstanding.
    [[nodiscard]] foundation::Result<void> replace(std::vector<launcher::DesktopFileId> pins);

    [[nodiscard]] const std::shared_ptr<const PinStateSnapshot> &current() const noexcept {
        return current_;
    }
    [[nodiscard]] const std::optional<foundation::Error> &lastError() const noexcept {
        return last_error_;
    }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

  signals:
    void snapshotChanged();
    void persistenceFailed();

  private:
    enum class Operation : std::uint8_t { open, replace };

    struct Work final {
        Operation operation;
        std::vector<launcher::DesktopFileId> pins;
    };

    AsyncPinStateController(std::filesystem::path prismdrakeStateDirectory,
                            std::unique_ptr<PinStatePersistenceBackend> backend, QObject *parent);

    [[nodiscard]] foundation::Result<void> submit(Work work);
    void workerMain(std::stop_token stopToken) noexcept;
    void complete(foundation::Result<std::shared_ptr<const PinStateSnapshot>> result);
    [[nodiscard]] bool onOwnerThread() const noexcept;

    std::filesystem::path state_directory_;
    std::unique_ptr<PinStatePersistenceBackend> backend_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Work> pending_;
    bool stopping_{false};
    std::jthread worker_;

    std::shared_ptr<const PinStateSnapshot> current_;
    std::optional<foundation::Error> last_error_;
    bool busy_{false};
    bool started_{false};
};

} // namespace prismdrake::shell::panel
