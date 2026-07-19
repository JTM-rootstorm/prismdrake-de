#include "QtSessionBusDispatcher.hpp"

#include <QSocketNotifier>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <limits>
#include <poll.h>
#include <utility>

namespace prismdrake::shell::controlclients::detail {
namespace {

constexpr int maximumMessagesPerTurn = 32;

[[nodiscard]] foundation::Error transportError() {
    return {foundation::ErrorCode::io_error, "The session bus connection failed.",
            "Retry after a private session bus is available."};
}

} // namespace

QtSessionBusDispatcher::QtSessionBusDispatcher(FailureCallback failureCallback)
    : failure_callback_(std::move(failureCallback)) {
    timer_.setSingleShot(true);
    QObject::connect(&timer_, &QTimer::timeout, this, [this]() noexcept { dispatch(); });
}

QtSessionBusDispatcher::~QtSessionBusDispatcher() { stop(); }

foundation::Result<void> QtSessionBusDispatcher::start() {
    if (bus_) {
        return foundation::Result<void>::failure({foundation::ErrorCode::invalid_argument,
                                                  "The session bus client is already running.",
                                                  "Stop the client before starting it again."});
    }

    sd_bus *opened = nullptr;
    if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
        return foundation::Result<void>::failure(transportError());
    }
    bus_.reset(opened);
    if (sd_bus_set_exit_on_disconnect(bus_.get(), 0) < 0) {
        stop();
        return foundation::Result<void>::failure(transportError());
    }

    const auto descriptor = sd_bus_get_fd(bus_.get());
    if (descriptor < 0) {
        stop();
        return foundation::Result<void>::failure(transportError());
    }
    read_notifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Read, this);
    write_notifier_ = std::make_unique<QSocketNotifier>(descriptor, QSocketNotifier::Write, this);
    QObject::connect(read_notifier_.get(), &QSocketNotifier::activated, this,
                     [this]() noexcept { dispatch(); });
    QObject::connect(write_notifier_.get(), &QSocketNotifier::activated, this,
                     [this]() noexcept { dispatch(); });
    refresh();
    return foundation::Result<void>::success();
}

void QtSessionBusDispatcher::stop() noexcept {
    timer_.stop();
    read_notifier_.reset();
    write_notifier_.reset();
    bus_.reset();
    dispatch_queued_ = false;
    failing_ = false;
}

void QtSessionBusDispatcher::dispatch() noexcept {
    if (!bus_ || failing_) {
        return;
    }
    if (read_notifier_) {
        read_notifier_->setEnabled(false);
    }
    if (write_notifier_) {
        write_notifier_->setEnabled(false);
    }

    int processed = 0;
    while (bus_ && processed < maximumMessagesPerTurn) {
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
    if (processed == maximumMessagesPerTurn && !dispatch_queued_) {
        dispatch_queued_ = true;
        QTimer::singleShot(0, this, [this]() noexcept {
            dispatch_queued_ = false;
            dispatch();
        });
    }
    refresh();
}

void QtSessionBusDispatcher::refresh() noexcept {
    if (!bus_ || failing_) {
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

void QtSessionBusDispatcher::fail() noexcept {
    if (failing_) {
        return;
    }
    failing_ = true;
    timer_.stop();
    read_notifier_.reset();
    write_notifier_.reset();
    bus_.reset();
    dispatch_queued_ = false;
    try {
        if (failure_callback_) {
            failure_callback_();
        }
    } catch (...) {
    }
    failing_ = false;
}

} // namespace prismdrake::shell::controlclients::detail
