#include "ServiceWait.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace prismdrake::settingsd {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::uint64_t microsecondsPerSecond = 1'000'000U;
constexpr std::uint64_t nanosecondsPerMicrosecond = 1'000U;
constexpr long nanosecondsPerSecond = 1'000'000'000L;
constexpr short descriptorFailureEvents = POLLERR | POLLHUP | POLLNVAL;

[[nodiscard]] foundation::Error waitError(std::string message) {
    return {ErrorCode::io_error, std::move(message), "Begin a fresh settings service owner epoch."};
}

[[nodiscard]] bool hasUnexpectedEvents(short observed, short permitted) noexcept {
    return (observed & static_cast<short>(~permitted)) != 0;
}

} // namespace

Result<ServiceWaitTimeout> serviceWaitTimeout(std::uint64_t busDeadlineMicroseconds,
                                              timespec monotonicNow) {
    if (busDeadlineMicroseconds == std::numeric_limits<std::uint64_t>::max()) {
        return Result<ServiceWaitTimeout>::success(ServiceWaitTimeout{true, {}});
    }
    if (monotonicNow.tv_sec < 0 || monotonicNow.tv_nsec < 0 ||
        monotonicNow.tv_nsec >= nanosecondsPerSecond) {
        return Result<ServiceWaitTimeout>::failure(
            waitError("The monotonic clock returned an invalid timestamp."));
    }

    const auto seconds = static_cast<std::uint64_t>(monotonicNow.tv_sec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() / microsecondsPerSecond) {
        return Result<ServiceWaitTimeout>::failure(
            waitError("The monotonic clock timestamp exceeds the supported wait range."));
    }
    const auto wholeMicroseconds = seconds * microsecondsPerSecond;
    const auto fractionalMicroseconds =
        static_cast<std::uint64_t>(monotonicNow.tv_nsec) / nanosecondsPerMicrosecond;
    if (wholeMicroseconds > std::numeric_limits<std::uint64_t>::max() - fractionalMicroseconds) {
        return Result<ServiceWaitTimeout>::failure(
            waitError("The monotonic clock timestamp exceeds the supported wait range."));
    }
    const auto nowMicroseconds = wholeMicroseconds + fractionalMicroseconds;
    if (busDeadlineMicroseconds <= nowMicroseconds) {
        return Result<ServiceWaitTimeout>::success(ServiceWaitTimeout{false, {0, 0}});
    }

    const auto remainingMicroseconds = busDeadlineMicroseconds - nowMicroseconds;
    const auto remainingSeconds = remainingMicroseconds / microsecondsPerSecond;
    if (remainingSeconds > static_cast<std::uint64_t>(std::numeric_limits<time_t>::max())) {
        return Result<ServiceWaitTimeout>::failure(
            waitError("The session bus deadline exceeds the supported wait range."));
    }
    const auto remainingNanoseconds =
        (remainingMicroseconds % microsecondsPerSecond) * nanosecondsPerMicrosecond;
    return Result<ServiceWaitTimeout>::success(ServiceWaitTimeout{
        false, {static_cast<time_t>(remainingSeconds), static_cast<long>(remainingNanoseconds)}});
}

ServiceWaitEvent classifyServiceWait(int waitResult, int waitErrorNumber, short busRequestedEvents,
                                     const pollfd &busDescriptor, const pollfd &workerDescriptor,
                                     const pollfd &shutdownDescriptor) noexcept {
    if (waitResult < 0) {
        return waitErrorNumber == EINTR ? ServiceWaitEvent::continue_processing
                                        : ServiceWaitEvent::disconnected;
    }
    if (waitResult == 0) {
        return ServiceWaitEvent::continue_processing;
    }

    constexpr short wakeEvents = POLLIN;
    const short busPermittedEvents =
        static_cast<short>(busRequestedEvents | descriptorFailureEvents);
    const short wakePermittedEvents = static_cast<short>(wakeEvents | descriptorFailureEvents);
    if (hasUnexpectedEvents(busDescriptor.revents, busPermittedEvents) ||
        hasUnexpectedEvents(workerDescriptor.revents, wakePermittedEvents) ||
        hasUnexpectedEvents(shutdownDescriptor.revents, wakePermittedEvents) ||
        (busDescriptor.revents & descriptorFailureEvents) != 0 ||
        (workerDescriptor.revents & descriptorFailureEvents) != 0 ||
        (shutdownDescriptor.revents & descriptorFailureEvents) != 0) {
        return ServiceWaitEvent::disconnected;
    }
    if ((shutdownDescriptor.revents & POLLIN) != 0) {
        return ServiceWaitEvent::shutdown;
    }
    if ((workerDescriptor.revents & POLLIN) != 0) {
        return ServiceWaitEvent::worker_completion;
    }
    return ServiceWaitEvent::continue_processing;
}

Result<std::unique_ptr<ServiceShutdownWake>> ServiceShutdownWake::create() {
    const int descriptor = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) {
        return Result<std::unique_ptr<ServiceShutdownWake>>::failure(
            waitError("Unable to create the settings service shutdown notification: " +
                      std::error_code(errno, std::generic_category()).message() + "."));
    }
    return Result<std::unique_ptr<ServiceShutdownWake>>::success(
        std::unique_ptr<ServiceShutdownWake>(new ServiceShutdownWake(descriptor)));
}

ServiceShutdownWake::~ServiceShutdownWake() {
    if (file_descriptor_ >= 0) {
        (void)::close(file_descriptor_);
        file_descriptor_ = -1;
    }
}

void ServiceShutdownWake::drain() noexcept {
    std::uint64_t notificationCount = 0U;
    while (::read(file_descriptor_, &notificationCount, sizeof(notificationCount)) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

void notifyServiceShutdown(int fileDescriptor) noexcept {
    if (fileDescriptor < 0) {
        return;
    }
    constexpr std::uint64_t notificationCount = 1U;
    while (::write(fileDescriptor, &notificationCount, sizeof(notificationCount)) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
}

} // namespace prismdrake::settingsd
