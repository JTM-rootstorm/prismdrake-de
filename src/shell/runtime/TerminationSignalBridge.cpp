#include "TerminationSignalBridge.hpp"

#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <unistd.h>
#include <utility>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;

volatile std::sig_atomic_t signal_write_fd = -1;

extern "C" void handleTerminationSignal(int signalNumber) {
    const auto savedErrno = errno;
    const auto fd = static_cast<int>(signal_write_fd);
    if (fd >= 0) {
        const auto value = static_cast<std::uint8_t>(signalNumber);
        static_cast<void>(::write(fd, &value, sizeof(value)));
    }
    errno = savedErrno;
}

[[nodiscard]] Result<void> setDescriptorFlags(int descriptor) {
    const auto statusFlags = ::fcntl(descriptor, F_GETFL);
    if (statusFlags < 0 || ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) < 0) {
        return Result<void>::failure(
            {ErrorCode::io_error, "The shell termination channel could not be made non-blocking.",
             "Restart prismdrake-shell after checking process descriptor limits."});
    }
    const auto descriptorFlags = ::fcntl(descriptor, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        return Result<void>::failure(
            {ErrorCode::io_error, "The shell termination channel could not be isolated.",
             "Restart prismdrake-shell after checking process descriptor limits."});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> signalInstallationError() {
    return Result<void>::failure(
        {ErrorCode::io_error, "The shell termination handlers could not be installed.",
         "Restart prismdrake-shell in a session that permits standard signal handling."});
}

} // namespace

TerminationSignalBridge::TerminationSignalBridge(Callback callback)
    : callback_(std::move(callback)) {}

TerminationSignalBridge::~TerminationSignalBridge() {
    if (terminate_installed_) {
        static_cast<void>(::sigaction(SIGTERM, &previous_terminate_, nullptr));
    }
    if (interrupt_installed_) {
        static_cast<void>(::sigaction(SIGINT, &previous_interrupt_, nullptr));
    }
    signal_write_fd = -1;
    notifier_.reset();
    if (read_fd_ >= 0) {
        static_cast<void>(::close(read_fd_));
    }
    if (write_fd_ >= 0) {
        static_cast<void>(::close(write_fd_));
    }
}

Result<std::unique_ptr<TerminationSignalBridge>>
TerminationSignalBridge::create(Callback callback) {
    std::unique_ptr<TerminationSignalBridge> bridge;
    try {
        bridge = std::unique_ptr<TerminationSignalBridge>(
            new TerminationSignalBridge(std::move(callback)));
    } catch (const std::bad_alloc &) {
        return Result<std::unique_ptr<TerminationSignalBridge>>::failure(
            {ErrorCode::too_large, "The shell termination bridge could not be allocated.",
             "Reduce memory pressure before restarting prismdrake-shell."});
    }
    auto initialized = bridge->initialize();
    if (!initialized) {
        return Result<std::unique_ptr<TerminationSignalBridge>>::failure(initialized.error());
    }
    return Result<std::unique_ptr<TerminationSignalBridge>>::success(std::move(bridge));
}

Result<void> TerminationSignalBridge::initialize() {
    if (!callback_ || signal_write_fd >= 0) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The shell termination bridge is not available.",
             "Create exactly one bridge with a valid owner-thread callback."});
    }

    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) < 0) {
        return Result<void>::failure(
            {ErrorCode::io_error, "The shell termination channel could not be created.",
             "Restart prismdrake-shell after checking process descriptor limits."});
    }
    read_fd_ = descriptors[0];
    write_fd_ = descriptors[1];
    if (auto configured = setDescriptorFlags(read_fd_); !configured) {
        return configured;
    }
    if (auto configured = setDescriptorFlags(write_fd_); !configured) {
        return configured;
    }

    try {
        notifier_ = std::make_unique<QSocketNotifier>(read_fd_, QSocketNotifier::Read, this);
    } catch (const std::bad_alloc &) {
        return Result<void>::failure(
            {ErrorCode::too_large, "The shell termination notifier could not be allocated.",
             "Reduce memory pressure before restarting prismdrake-shell."});
    }
    connect(notifier_.get(), &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { drainSignalPipe(); });

    struct sigaction action{};
    action.sa_handler = handleTerminationSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    signal_write_fd = write_fd_;
    if (::sigaction(SIGINT, &action, &previous_interrupt_) < 0) {
        signal_write_fd = -1;
        return signalInstallationError();
    }
    interrupt_installed_ = true;
    if (::sigaction(SIGTERM, &action, &previous_terminate_) < 0) {
        static_cast<void>(::sigaction(SIGINT, &previous_interrupt_, nullptr));
        interrupt_installed_ = false;
        signal_write_fd = -1;
        return signalInstallationError();
    }
    terminate_installed_ = true;
    return Result<void>::success();
}

void TerminationSignalBridge::drainSignalPipe() {
    std::uint8_t buffer[64];
    while (::read(read_fd_, buffer, sizeof(buffer)) > 0) {
    }
    if (!callback_requested_) {
        callback_requested_ = true;
        notifier_->setEnabled(false);
        callback_();
    }
}

} // namespace prismdrake::shell::runtime
