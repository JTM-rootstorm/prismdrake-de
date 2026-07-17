#pragma once

#include "Result.hpp"
#include "SettingsEngine.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

namespace prismdrake::settingsd {

using WorkerRequestId = std::uint64_t;

struct ProfileChangeJob final {
    std::string profileId;
};

struct ReloadJob final {};

struct CandidateValidationJob final {
    std::string candidateToml;
};

using WorkerOperation = std::variant<ProfileChangeJob, ReloadJob, CandidateValidationJob>;

struct WorkerJob final {
    WorkerRequestId requestId;
    WorkerOperation operation;
};

using PublicationResult = foundation::Result<settings::PublicationOutcome>;
using CandidateValidationResult = foundation::Result<settings::CandidateValidation>;
using WorkerResult = std::variant<PublicationResult, CandidateValidationResult>;

/// Bus-independent result made available to the dispatch loop after one job completes.
struct WorkerCompletion final {
    WorkerRequestId requestId;
    WorkerResult result;
};

/// Single-slot owner of SettingsEngine and all potentially blocking settings work.
///
/// A queued job, an executing job, or an untaken completion occupies the one slot.
/// Callers must not submit another job until takeCompletion() releases that slot.
class ServiceWorker final {
  public:
    /// Creates a worker around an already-started engine and its generation-one snapshot.
    [[nodiscard]] static foundation::Result<std::unique_ptr<ServiceWorker>>
    create(std::unique_ptr<settings::SettingsEngine> engine);

    ~ServiceWorker();

    ServiceWorker(const ServiceWorker &) = delete;
    ServiceWorker &operator=(const ServiceWorker &) = delete;
    ServiceWorker(ServiceWorker &&) = delete;
    ServiceWorker &operator=(ServiceWorker &&) = delete;

    /// Returns the eventfd to poll for a completed job. The worker retains ownership.
    [[nodiscard]] int notificationFileDescriptor() const noexcept { return notification_fd_; }

    /// Submits one job, returning false when the slot is occupied or shutdown has begun.
    [[nodiscard]] bool trySubmit(WorkerJob job);

    /// Drains the completion notification and releases an occupied completion slot.
    [[nodiscard]] std::optional<WorkerCompletion> takeCompletion() noexcept;

    /// Returns an immutable snapshot whose lifetime is independent of subsequent publications.
    [[nodiscard]] std::shared_ptr<const settings::SettingsSnapshot> currentSnapshot() const;

    /// Stops accepting work, discards a job that has not begun, and joins the worker thread.
    void stop() noexcept;

  private:
    ServiceWorker(std::unique_ptr<settings::SettingsEngine> engine, int notificationFd);

    [[nodiscard]] WorkerCompletion execute(WorkerJob job);
    void run();
    void notifyCompletion() noexcept;
    void drainNotification() noexcept;

    std::unique_ptr<settings::SettingsEngine> engine_;
    int notification_fd_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<WorkerJob> pending_job_;
    std::optional<WorkerCompletion> completion_;
    std::shared_ptr<const settings::SettingsSnapshot> current_snapshot_;
    bool processing_ = false;
    bool stopping_ = false;
    std::jthread thread_;
};

} // namespace prismdrake::settingsd
