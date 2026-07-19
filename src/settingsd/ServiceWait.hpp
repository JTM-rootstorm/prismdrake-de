#pragma once

#include "Result.hpp"

#include <csignal>
#include <cstdint>
#include <memory>

#include <poll.h>
#include <time.h>

namespace prismdrake::settingsd {

/// Relative ppoll timeout derived from the absolute sd-bus monotonic deadline.
struct ServiceWaitTimeout final {
    bool infinite = false;
    timespec duration{};
};

/// Computes an exact microsecond-resolution relative wait from an sd-bus deadline.
/// UINT64_MAX means that the bus has no deadline and permits an infinite wait.
[[nodiscard]] foundation::Result<ServiceWaitTimeout>
serviceWaitTimeout(std::uint64_t busDeadlineMicroseconds, timespec monotonicNow);

enum class ServiceWaitEvent : std::uint8_t {
    continue_processing,
    worker_completion,
    shutdown,
    disconnected,
};

/// Classifies one ppoll result. Descriptor errors and unexpected event bits fail closed.
[[nodiscard]] ServiceWaitEvent classifyServiceWait(int waitResult, int waitError,
                                                   short busRequestedEvents,
                                                   const pollfd &busDescriptor,
                                                   const pollfd &workerDescriptor,
                                                   const pollfd &shutdownDescriptor) noexcept;

/// Process-owned eventfd used to wake an otherwise infinite service wait on shutdown.
class ServiceShutdownWake final {
  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<ServiceShutdownWake>> create();

    ~ServiceShutdownWake();

    ServiceShutdownWake(const ServiceShutdownWake &) = delete;
    ServiceShutdownWake &operator=(const ServiceShutdownWake &) = delete;
    ServiceShutdownWake(ServiceShutdownWake &&) = delete;
    ServiceShutdownWake &operator=(ServiceShutdownWake &&) = delete;

    [[nodiscard]] int fileDescriptor() const noexcept { return file_descriptor_; }
    void drain() noexcept;

  private:
    explicit ServiceShutdownWake(int fileDescriptor) : file_descriptor_(fileDescriptor) {}

    int file_descriptor_;
};

/// Async-signal-safe notification for a ServiceShutdownWake descriptor.
void notifyServiceShutdown(int fileDescriptor) noexcept;

} // namespace prismdrake::settingsd
