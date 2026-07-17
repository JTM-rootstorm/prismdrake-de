#include "ServiceWorker.hpp"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <type_traits>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace prismdrake::settingsd {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error eventFileDescriptorError(int errorNumber) {
    return {ErrorCode::io_error,
            "Unable to create the settings worker completion notification: " +
                std::error_code(errorNumber, std::generic_category()).message() + ".",
            "Check the process file-descriptor limit and retry starting prismdrake-settingsd."};
}

[[nodiscard]] foundation::Error workerThreadError(const std::system_error &error) {
    return {ErrorCode::io_error,
            "Unable to start the settings worker thread: " + error.code().message() + ".",
            "Check the process thread limit and retry starting prismdrake-settingsd."};
}

} // namespace

Result<std::unique_ptr<ServiceWorker>>
ServiceWorker::create(std::unique_ptr<settings::SettingsEngine> engine) {
    if (!engine || !engine->current()) {
        return Result<std::unique_ptr<ServiceWorker>>::failure(
            {ErrorCode::invalid_argument,
             "The settings worker requires a started engine with a current snapshot.",
             "Start SettingsEngine successfully before creating the service worker."});
    }

    const int notificationFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (notificationFd < 0) {
        return Result<std::unique_ptr<ServiceWorker>>::failure(eventFileDescriptorError(errno));
    }

    auto worker =
        std::unique_ptr<ServiceWorker>(new ServiceWorker(std::move(engine), notificationFd));
    try {
        worker->thread_ = std::jthread([workerPointer = worker.get()] { workerPointer->run(); });
    } catch (const std::system_error &error) {
        return Result<std::unique_ptr<ServiceWorker>>::failure(workerThreadError(error));
    }
    return Result<std::unique_ptr<ServiceWorker>>::success(std::move(worker));
}

ServiceWorker::ServiceWorker(std::unique_ptr<settings::SettingsEngine> engine, int notificationFd)
    : engine_(std::move(engine)), notification_fd_(notificationFd),
      current_snapshot_(engine_->current()) {}

ServiceWorker::~ServiceWorker() {
    stop();
    if (notification_fd_ >= 0) {
        (void)::close(notification_fd_);
        notification_fd_ = -1;
    }
}

bool ServiceWorker::trySubmit(WorkerJob job) {
    std::lock_guard lock(mutex_);
    if (stopping_ || pending_job_.has_value() || processing_ || completion_.has_value()) {
        return false;
    }
    pending_job_.emplace(std::move(job));
    condition_.notify_one();
    return true;
}

std::optional<WorkerCompletion> ServiceWorker::takeCompletion() noexcept {
    drainNotification();
    std::lock_guard lock(mutex_);
    if (!completion_) {
        return std::nullopt;
    }
    auto completion = std::move(completion_);
    completion_.reset();
    return completion;
}

std::shared_ptr<const settings::SettingsSnapshot> ServiceWorker::currentSnapshot() const {
    std::lock_guard lock(mutex_);
    return current_snapshot_;
}

void ServiceWorker::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        pending_job_.reset();
    }
    thread_.request_stop();
    condition_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

WorkerCompletion ServiceWorker::execute(WorkerJob job) {
    const WorkerRequestId requestId = job.requestId;
    auto result = std::visit(
        [this](auto &&operation) -> WorkerResult {
            using Operation = std::remove_cvref_t<decltype(operation)>;
            if constexpr (std::is_same_v<Operation, ProfileChangeJob>) {
                return engine_->requestProfileChange(operation.profileId);
            } else if constexpr (std::is_same_v<Operation, ReloadJob>) {
                return engine_->reload();
            } else {
                return engine_->validateCandidate(operation.candidateToml);
            }
        },
        std::move(job.operation));
    return {requestId, std::move(result)};
}

void ServiceWorker::run() {
    while (true) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || pending_job_.has_value(); });
        if (stopping_) {
            return;
        }

        auto pendingJob = std::exchange(pending_job_, std::nullopt);
        if (!pendingJob) {
            continue;
        }
        auto job = std::move(pendingJob).value();
        processing_ = true;
        lock.unlock();

        auto completion = execute(std::move(job));

        lock.lock();
        processing_ = false;
        if (const auto *publication = std::get_if<PublicationResult>(&completion.result);
            publication != nullptr && publication->hasValue()) {
            current_snapshot_ = publication->value().snapshot;
        }
        if (stopping_) {
            return;
        }
        completion_.emplace(std::move(completion));
        lock.unlock();
        notifyCompletion();
    }
}

void ServiceWorker::notifyCompletion() noexcept {
    constexpr std::uint64_t completionCount = 1U;
    while (::write(notification_fd_, &completionCount, sizeof(completionCount)) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
}

void ServiceWorker::drainNotification() noexcept {
    std::uint64_t completionCount = 0;
    while (::read(notification_fd_, &completionCount, sizeof(completionCount)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

} // namespace prismdrake::settingsd
