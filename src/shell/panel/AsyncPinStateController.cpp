#include "AsyncPinStateController.hpp"

#include <QMetaObject>
#include <QThread>

#include <exception>
#include <new>
#include <utility>

namespace prismdrake::shell::panel {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using Snapshot = std::shared_ptr<const PinStateSnapshot>;

[[nodiscard]] foundation::Error controllerError(ErrorCode code, std::string message,
                                                std::string recovery) {
    return {code, std::move(message), std::move(recovery)};
}

class FilePinStatePersistenceBackend final : public PinStatePersistenceBackend {
  public:
    Result<Snapshot> open(const std::filesystem::path &prismdrakeStateDirectory) override {
        if (store_) {
            return Result<Snapshot>::failure(controllerError(
                ErrorCode::invalid_argument, "The pin-state backend is already open.",
                "Open one private pin-state store per shell owner epoch."));
        }
        auto opened = PinStateStore::openInStateDirectory(prismdrakeStateDirectory);
        if (!opened) {
            return Result<Snapshot>::failure(opened.error());
        }
        store_.emplace(std::move(opened).value());
        return Result<Snapshot>::success(store_->current());
    }

    Result<Snapshot> replace(std::vector<launcher::DesktopFileId> pins) override {
        if (!store_) {
            return Result<Snapshot>::failure(controllerError(
                ErrorCode::cancelled, "The pin-state backend is not open.",
                "Wait for the current private pin-state snapshot before replacing it."));
        }
        return store_->replace(std::move(pins));
    }

  private:
    std::optional<PinStateStore> store_;
};

[[nodiscard]] Result<void> ownerThreadFailure() {
    return Result<void>::failure(
        controllerError(ErrorCode::cancelled,
                        "The asynchronous pin-state controller was called off its owner thread.",
                        "Queue pin-state requests to the controller QObject thread."));
}

[[nodiscard]] Result<void> stoppedFailure() {
    return Result<void>::failure(
        controllerError(ErrorCode::cancelled, "The asynchronous pin-state controller is stopped.",
                        "Create a new controller for the next shell owner epoch."));
}

[[nodiscard]] Result<Snapshot> unexpectedBackendFailure() {
    return Result<Snapshot>::failure(controllerError(
        ErrorCode::io_error, "The pin-state persistence backend failed unexpectedly.",
        "Retain the prior complete snapshot and restart the shell persistence owner."));
}

} // namespace

Result<std::unique_ptr<AsyncPinStateController>>
AsyncPinStateController::create(std::filesystem::path prismdrakeStateDirectory, QObject *parent) {
    return create(std::move(prismdrakeStateDirectory),
                  std::make_unique<FilePinStatePersistenceBackend>(), parent);
}

Result<std::unique_ptr<AsyncPinStateController>>
AsyncPinStateController::create(std::filesystem::path prismdrakeStateDirectory,
                                std::unique_ptr<PinStatePersistenceBackend> backend,
                                QObject *parent) {
    if (prismdrakeStateDirectory.empty() || !prismdrakeStateDirectory.is_absolute() || !backend) {
        return Result<std::unique_ptr<AsyncPinStateController>>::failure(controllerError(
            ErrorCode::invalid_argument, "The asynchronous pin-state configuration is invalid.",
            "Use an absolute Prismdrake XDG state path and one persistence backend."));
    }
    try {
        return Result<std::unique_ptr<AsyncPinStateController>>::success(
            std::unique_ptr<AsyncPinStateController>(new AsyncPinStateController(
                std::move(prismdrakeStateDirectory), std::move(backend), parent)));
    } catch (const std::exception &) {
        return Result<std::unique_ptr<AsyncPinStateController>>::failure(controllerError(
            ErrorCode::io_error, "The asynchronous pin-state worker could not be created.",
            "Restart the shell after checking process resource limits."));
    }
}

AsyncPinStateController::AsyncPinStateController(
    std::filesystem::path prismdrakeStateDirectory,
    std::unique_ptr<PinStatePersistenceBackend> backend, QObject *parent)
    : QObject(parent), state_directory_(std::move(prismdrakeStateDirectory)),
      backend_(std::move(backend)), worker_([this](std::stop_token token) { workerMain(token); }) {}

AsyncPinStateController::~AsyncPinStateController() { stop(); }

bool AsyncPinStateController::onOwnerThread() const noexcept {
    return QThread::currentThread() == thread();
}

Result<void> AsyncPinStateController::start() {
    if (!onOwnerThread()) {
        return ownerThreadFailure();
    }
    if (started_ || stopping_) {
        return stoppedFailure();
    }
    started_ = true;
    return submit({Operation::open, {}});
}

Result<void> AsyncPinStateController::replace(std::vector<launcher::DesktopFileId> pins) {
    if (!onOwnerThread()) {
        return ownerThreadFailure();
    }
    if (!started_ || stopping_ || !current_) {
        return stoppedFailure();
    }
    if (busy_) {
        return Result<void>::failure(controllerError(
            ErrorCode::cancelled, "A pin-state persistence operation is already pending.",
            "Wait for the current complete snapshot before another pin change."));
    }
    return submit({Operation::replace, std::move(pins)});
}

Result<void> AsyncPinStateController::submit(Work work) {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_ || pending_) {
            return stoppedFailure();
        }
        pending_.emplace(std::move(work));
    }
    busy_ = true;
    last_error_.reset();
    condition_.notify_one();
    return Result<void>::success();
}

void AsyncPinStateController::stop() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;
        }
        stopping_ = true;
        pending_.reset();
    }
    worker_.request_stop();
    backend_->requestStop();
    condition_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = false;
}

void AsyncPinStateController::workerMain(std::stop_token stopToken) noexcept {
    for (;;) {
        std::optional<Work> work;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this, &stopToken] {
                return stopping_ || stopToken.stop_requested() || pending_.has_value();
            });
            if (stopping_ || stopToken.stop_requested()) {
                return;
            }
            work = std::move(pending_);
            pending_.reset();
        }

        Result<Snapshot> result = unexpectedBackendFailure();
        try {
            result = work->operation == Operation::open ? backend_->open(state_directory_)
                                                        : backend_->replace(std::move(work->pins));
        } catch (...) {
            result = unexpectedBackendFailure();
        }
        if (stopToken.stop_requested()) {
            return;
        }
        try {
            static_cast<void>(QMetaObject::invokeMethod(
                this, [this, result = std::move(result)]() mutable { complete(std::move(result)); },
                Qt::QueuedConnection));
        } catch (...) {
            return;
        }
    }
}

void AsyncPinStateController::complete(Result<Snapshot> result) {
    if (!onOwnerThread() || stopping_) {
        return;
    }
    busy_ = false;
    if (!result) {
        last_error_ = result.error();
        emit persistenceFailed();
        return;
    }
    current_ = std::move(result).value();
    last_error_.reset();
    emit snapshotChanged();
}

} // namespace prismdrake::shell::panel
