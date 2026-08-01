#include "X11PropertyWorker.hpp"

#include "AtomCache.hpp"
#include "DesktopProperties.hpp"
#include "DockProperties.hpp"
#include "UnmappedWindowStateProperties.hpp"
#include "X11Connection.hpp"

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <utility>

#include <sys/socket.h>

namespace prismdrake::shell::window {
namespace {

using foundation::ErrorCode;
using foundation::Result;

inline constexpr std::size_t maximumQueuedPropertyTransactions = 64U;
inline constexpr std::size_t maximumPropertyOperationsPerTransaction = 128U;
inline constexpr std::size_t maximumUnmappedPublicationAttempts = 32U;
inline constexpr auto unmappedPublicationRetryDelay = std::chrono::milliseconds{5};

[[nodiscard]] Result<void> invalidJob() {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "The X11 property transaction is invalid.",
         "Submit one non-zero token and a bounded non-empty operation set."});
}

[[nodiscard]] Result<void> stoppedWorker() {
    return Result<void>::failure(
        {ErrorCode::cancelled, "The X11 property worker is unavailable.",
         "Discard the pending surfaces and rebuild them after X11 recovery."});
}

[[nodiscard]] bool isTransientUnmappedPublicationFailure(const Result<void> &result) noexcept {
    return !result && (result.error().code == ErrorCode::invalid_argument ||
                       result.error().code == ErrorCode::io_error);
}

[[nodiscard]] bool
isUnmappedPublication(std::span<const X11PropertyOperation> operations) noexcept {
    return !operations.empty() && std::ranges::all_of(operations, [](const auto &operation) {
        return operation.kind == X11PropertyOperationKind::publishUnmappedSkipTaskbar;
    });
}

template <typename Attempt>
[[nodiscard]] Result<void> retryUnmappedPublication(std::stop_token stopToken, Attempt &&attempt) {
    if (stopToken.stop_requested()) {
        return stoppedWorker();
    }
    auto result = attempt();
    std::size_t attempts = 1U;
    std::mutex retryMutex;
    std::condition_variable_any retryWake;
    while (attempts < maximumUnmappedPublicationAttempts &&
           isTransientUnmappedPublicationFailure(result)) {
        std::unique_lock lock{retryMutex};
        static_cast<void>(retryWake.wait_for(lock, stopToken, unmappedPublicationRetryDelay,
                                             [] { return false; }));
        if (stopToken.stop_requested()) {
            return stoppedWorker();
        }
        result = attempt();
        ++attempts;
    }
    return result;
}

[[nodiscard]] Result<void> applyOperation(x11::X11Connection &connection,
                                          const x11::AtomCache &atoms,
                                          const X11PropertyOperation &operation) {
    switch (operation.kind) {
    case X11PropertyOperationKind::publishDock:
        if (!operation.dockReservation) {
            return invalidJob();
        }
        return x11::DockProperties::publishBottomPanelReservation(
            connection, atoms, operation.window, *operation.dockReservation);
    case X11PropertyOperationKind::removeDock:
        return x11::DockProperties::remove(connection, atoms, operation.window);
    case X11PropertyOperationKind::publishDesktop:
        return x11::DesktopProperties::publish(connection, atoms, operation.window);
    case X11PropertyOperationKind::removeDesktop:
        return x11::DesktopProperties::remove(connection, atoms, operation.window);
    case X11PropertyOperationKind::publishUnmappedSkipTaskbar:
        return x11::UnmappedWindowStateProperties::publishSkipTaskbar(connection, atoms,
                                                                      operation.window);
    }
    return invalidJob();
}

[[nodiscard]] std::optional<X11PropertyOperation>
rollbackFor(const X11PropertyOperation &operation) {
    switch (operation.kind) {
    case X11PropertyOperationKind::publishDock:
        return X11PropertyOperation{X11PropertyOperationKind::removeDock, operation.window,
                                    std::nullopt};
    case X11PropertyOperationKind::publishDesktop:
        return X11PropertyOperation{X11PropertyOperationKind::removeDesktop, operation.window,
                                    std::nullopt};
    case X11PropertyOperationKind::removeDock:
    case X11PropertyOperationKind::removeDesktop:
    case X11PropertyOperationKind::publishUnmappedSkipTaskbar:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

Result<std::unique_ptr<X11PropertyWorker>> X11PropertyWorker::create(QObject &owner,
                                                                     std::string display,
                                                                     Completion completion,
                                                                     Transaction testTransaction) {
    if (display.empty() || !completion) {
        return Result<std::unique_ptr<X11PropertyWorker>>::failure(invalidJob().error());
    }
    try {
        return Result<std::unique_ptr<X11PropertyWorker>>::success(
            std::unique_ptr<X11PropertyWorker>(new X11PropertyWorker(
                owner, std::move(display), std::move(completion), std::move(testTransaction))));
    } catch (const std::exception &) {
        return Result<std::unique_ptr<X11PropertyWorker>>::failure(
            {ErrorCode::too_large, "The X11 property worker could not be allocated.",
             "Stop surface startup without publishing partial windows."});
    }
}

X11PropertyWorker::X11PropertyWorker(QObject &owner, std::string display, Completion completion,
                                     Transaction testTransaction)
    : owner_(owner), display_(std::move(display)), completion_(std::move(completion)),
      test_transaction_(std::move(testTransaction)),
      thread_([this](std::stop_token stopToken) { run(stopToken); }) {}

X11PropertyWorker::~X11PropertyWorker() {
    {
        std::scoped_lock lock{mutex_};
        stopping_ = true;
        jobs_.clear();
    }
    thread_.request_stop();
    wake_.notify_all();
    {
        std::scoped_lock lock{connection_mutex_};
        if (live_connection_ != nullptr) {
            const int descriptor = live_connection_->eventFileDescriptor();
            if (descriptor >= 0) {
                (void)shutdown(descriptor, SHUT_RDWR);
            }
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

Result<void> X11PropertyWorker::submit(std::uint64_t token,
                                       std::vector<X11PropertyOperation> operations) {
    if (token == 0U || operations.empty() ||
        operations.size() > maximumPropertyOperationsPerTransaction) {
        return invalidJob();
    }
    for (const auto &operation : operations) {
        if (operation.kind == X11PropertyOperationKind::publishUnmappedSkipTaskbar) {
            return invalidJob();
        }
        const bool reservationRequired = operation.kind == X11PropertyOperationKind::publishDock;
        if (operation.dockReservation.has_value() != reservationRequired) {
            return invalidJob();
        }
    }
    return enqueue(token, std::move(operations), true);
}

Result<void> X11PropertyWorker::submitUnmappedSkipTaskbar(std::uint64_t token,
                                                          std::vector<x11::WindowId> windows) {
    if (token == 0U || windows.empty() ||
        windows.size() > maximumPropertyOperationsPerTransaction) {
        return invalidJob();
    }
    std::set<x11::WindowId::Value> identifiers;
    const bool distinct = std::ranges::all_of(windows, [&identifiers](const auto window) {
        return identifiers.insert(window.value()).second;
    });
    if (!distinct) {
        return invalidJob();
    }
    std::vector<X11PropertyOperation> operations;
    operations.reserve(windows.size());
    for (const auto window : windows) {
        operations.push_back(
            {X11PropertyOperationKind::publishUnmappedSkipTaskbar, window, std::nullopt});
    }
    return enqueue(token, std::move(operations), false);
}

Result<void> X11PropertyWorker::enqueue(std::uint64_t token,
                                        std::vector<X11PropertyOperation> operations,
                                        bool rollbackOnFailure) {
    {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
            return stoppedWorker();
        }
        if (jobs_.size() >= maximumQueuedPropertyTransactions) {
            return Result<void>::failure(
                {ErrorCode::too_large, "The bounded X11 property queue is full.",
                 "Coalesce surface generations before submitting more work."});
        }
        jobs_.push_back({token, std::move(operations), rollbackOnFailure});
    }
    wake_.notify_one();
    return Result<void>::success();
}

void X11PropertyWorker::run(std::stop_token stopToken) {
    std::optional<x11::X11Connection> connection;
    std::optional<foundation::Error> setupError;
    std::optional<x11::AtomCache> atoms;
    if (!test_transaction_) {
        auto connected = x11::X11Connection::connect(display_);
        if (!connected) {
            setupError = connected.error();
        } else {
            connection = std::move(connected).value();
        }
    }
    if (connection) {
        {
            std::scoped_lock lock{connection_mutex_};
            live_connection_ = &*connection;
        }
        auto createdAtoms = x11::AtomCache::create(*connection);
        if (!createdAtoms) {
            setupError = createdAtoms.error();
        } else {
            atoms = std::move(createdAtoms).value();
        }
    }

    while (!stopToken.stop_requested()) {
        std::optional<Job> job;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, stopToken, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ || stopToken.stop_requested()) {
                break;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (test_transaction_) {
            auto execute = [this, &stopToken, &job] {
                return test_transaction_(stopToken, job->operations);
            };
            auto result = isUnmappedPublication(job->operations)
                              ? retryUnmappedPublication(stopToken, execute)
                              : execute();
            deliver(job->token, std::move(result));
            continue;
        }
        if (setupError) {
            deliver(job->token, Result<void>::failure(*setupError));
            continue;
        }
        std::vector<X11PropertyOperation> published;
        Result<void> result = Result<void>::success();
        for (const auto &operation : job->operations) {
            const auto apply = [&connection, &atoms, &operation] {
                return applyOperation(*connection, *atoms, operation);
            };
            result = operation.kind == X11PropertyOperationKind::publishUnmappedSkipTaskbar
                         ? retryUnmappedPublication(stopToken, apply)
                         : apply();
            if (!result) {
                break;
            }
            if (operation.kind == X11PropertyOperationKind::publishDock ||
                operation.kind == X11PropertyOperationKind::publishDesktop) {
                published.push_back(operation);
            }
        }
        if (!result && job->rollbackOnFailure) {
            for (auto operation = published.rbegin(); operation != published.rend(); ++operation) {
                if (const auto rollback = rollbackFor(*operation)) {
                    static_cast<void>(applyOperation(*connection, *atoms, *rollback));
                }
            }
        }
        deliver(job->token, std::move(result));
    }
    {
        std::scoped_lock lock{connection_mutex_};
        live_connection_ = nullptr;
    }
}

void X11PropertyWorker::deliver(std::uint64_t token, Result<void> result) {
    auto payload = std::make_shared<Result<void>>(std::move(result));
    const auto completion = completion_;
    QMetaObject::invokeMethod(
        &owner_, [completion, token, payload] { completion(token, std::move(*payload)); },
        Qt::QueuedConnection);
}

} // namespace prismdrake::shell::window
