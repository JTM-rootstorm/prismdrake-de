#include "ShellControlBusOwner.hpp"

#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <time.h>

namespace prismdrake::shell::runtime {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr char serviceName[] = "org.prismdrake.Shell1";
constexpr int maximumDispatchBatch = 32;

[[nodiscard]] Error transportError(ErrorCode code = ErrorCode::io_error) {
    return {code, "The Experimental shell-control bus owner is unavailable.",
            "Restart the shell inside a working user D-Bus session."};
}

} // namespace

ShellControlBusOwner::ShellControlBusOwner(control::ShellPresentationPort &presentationPort,
                                           FailureHandler failureHandler, QObject *parent)
    : QObject(parent), service_(presentationPort), failure_handler_(std::move(failureHandler)) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this]() noexcept { dispatch(); });
}

ShellControlBusOwner::~ShellControlBusOwner() { stop(); }

Result<void> ShellControlBusOwner::start() {
    if (start_attempted_) {
        return Result<void>::failure(transportError(ErrorCode::invalid_argument));
    }
    start_attempted_ = true;
    sd_bus *opened = nullptr;
    if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
        return Result<void>::failure(transportError());
    }
    bus_.reset(opened);
    if (sd_bus_set_exit_on_disconnect(bus_.get(), 0) < 0) {
        stop();
        return Result<void>::failure(transportError());
    }
    auto attached = service_.attach(bus_.get());
    if (!attached) {
        stop();
        return attached;
    }
    const int owned = sd_bus_request_name(bus_.get(), serviceName, 0U);
    if (owned < 0) {
        stop();
        return Result<void>::failure(transportError(
            owned == -EEXIST ? ErrorCode::invalid_environment : ErrorCode::io_error));
    }
    const auto descriptor = sd_bus_get_fd(bus_.get());
    if (descriptor < 0) {
        stop();
        return Result<void>::failure(transportError());
    }
    read_notifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Read, this);
    write_notifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Write, this);
    connect(read_notifier_.get(), &QSocketNotifier::activated, this,
            [this]() noexcept { dispatch(); });
    connect(write_notifier_.get(), &QSocketNotifier::activated, this,
            [this]() noexcept { dispatch(); });
    updateEventSources();
    return Result<void>::success();
}

void ShellControlBusOwner::stop() noexcept {
    start_attempted_ = true;
    service_.beginServiceStop();
    timer_.stop();
    read_notifier_.reset();
    write_notifier_.reset();
    bus_.reset();
    dispatch_queued_ = false;
}

void ShellControlBusOwner::dispatch() noexcept {
    if (!bus_) {
        return;
    }
    if (read_notifier_) {
        read_notifier_->setEnabled(false);
    }
    if (write_notifier_) {
        write_notifier_->setEnabled(false);
    }
    int processed = 0;
    while (processed < maximumDispatchBatch) {
        const auto result = sd_bus_process(bus_.get(), nullptr);
        if (result < 0) {
            fail();
            return;
        }
        if (result == 0) {
            break;
        }
        ++processed;
    }
    if (processed == maximumDispatchBatch && !dispatch_queued_) {
        dispatch_queued_ = true;
        QTimer::singleShot(0, this, [this]() noexcept {
            dispatch_queued_ = false;
            dispatch();
        });
    }
    updateEventSources();
}

void ShellControlBusOwner::updateEventSources() noexcept {
    if (!bus_) {
        return;
    }
    const auto events = sd_bus_get_events(bus_.get());
    if (events < 0) {
        fail();
        return;
    }
    if (read_notifier_) {
        read_notifier_->setEnabled((events & POLLIN) != 0);
    }
    if (write_notifier_) {
        write_notifier_->setEnabled((events & POLLOUT) != 0);
    }
    std::uint64_t timeout = 0U;
    if (sd_bus_get_timeout(bus_.get(), &timeout) < 0) {
        fail();
        return;
    }
    if (timeout == std::numeric_limits<std::uint64_t>::max()) {
        timer_.stop();
        return;
    }
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 || now.tv_nsec < 0) {
        fail();
        return;
    }
    const auto seconds = static_cast<std::uint64_t>(now.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(now.tv_nsec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000U) {
        fail();
        return;
    }
    const auto nowUsec = seconds * 1'000'000U + nanoseconds / 1'000U;
    const auto delayUsec = timeout > nowUsec ? timeout - nowUsec : 0U;
    const auto delayMs =
        std::min<std::uint64_t>((delayUsec + 999U) / 1'000U, std::numeric_limits<int>::max());
    timer_.start(static_cast<int>(delayMs));
}

void ShellControlBusOwner::fail() noexcept {
    const auto error = transportError();
    stop();
    if (failure_handler_) {
        failure_handler_(error);
    }
}

} // namespace prismdrake::shell::runtime
