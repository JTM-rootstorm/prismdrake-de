#include "ServiceWait.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <limits>

#include <poll.h>

namespace prismdrake::settingsd {
namespace {

[[nodiscard]] pollfd descriptor(int fileDescriptor, short events, short returnedEvents = 0) {
    return {fileDescriptor, events, returnedEvents};
}

TEST(SettingsServiceWaitTest, AllowsInfiniteIdleWhenTheBusHasNoDeadline) {
    const auto timeout = serviceWaitTimeout(std::numeric_limits<std::uint64_t>::max(), {});
    ASSERT_TRUE(timeout);
    EXPECT_TRUE(timeout.value().infinite);
}

TEST(SettingsServiceWaitTest, ReportsDueAndExactFiniteBusDeadlines) {
    auto timeout = serviceWaitTimeout(2'500'000U, timespec{2, 500'000'000L});
    ASSERT_TRUE(timeout);
    EXPECT_FALSE(timeout.value().infinite);
    EXPECT_EQ(timeout.value().duration.tv_sec, 0);
    EXPECT_EQ(timeout.value().duration.tv_nsec, 0);

    timeout = serviceWaitTimeout(3'750'125U, timespec{2, 500'000'000L});
    ASSERT_TRUE(timeout);
    EXPECT_FALSE(timeout.value().infinite);
    EXPECT_EQ(timeout.value().duration.tv_sec, 1);
    EXPECT_EQ(timeout.value().duration.tv_nsec, 250'125'000L);
}

TEST(SettingsServiceWaitTest, RejectsInvalidOrOverflowingClockObservations) {
    EXPECT_FALSE(serviceWaitTimeout(1U, timespec{-1, 0}));
    EXPECT_FALSE(serviceWaitTimeout(1U, timespec{0, 1'000'000'000L}));
    const auto maximumTimeSeconds = static_cast<std::uint64_t>(std::numeric_limits<time_t>::max());
    if (maximumTimeSeconds > std::numeric_limits<std::uint64_t>::max() / 1'000'000U) {
        EXPECT_FALSE(
            serviceWaitTimeout(1U, timespec{std::numeric_limits<time_t>::max(), 999'999'999L}));
    } else {
        EXPECT_FALSE(serviceWaitTimeout(std::numeric_limits<std::uint64_t>::max() - 1U, {}));
    }
}

TEST(SettingsServiceWaitTest, ClassifiesTimeoutBusProgressAndWorkerCompletion) {
    const auto bus = descriptor(3, static_cast<short>(POLLIN | POLLOUT), POLLIN);
    const auto idleWorker = descriptor(4, POLLIN);
    const auto idleShutdown = descriptor(5, POLLIN);
    EXPECT_EQ(classifyServiceWait(0, 0, bus.events, bus, idleWorker, idleShutdown),
              ServiceWaitEvent::continue_processing);
    EXPECT_EQ(classifyServiceWait(1, 0, bus.events, bus, idleWorker, idleShutdown),
              ServiceWaitEvent::continue_processing);

    const auto readyWorker = descriptor(4, POLLIN, POLLIN);
    EXPECT_EQ(
        classifyServiceWait(1, 0, bus.events, descriptor(3, bus.events), readyWorker, idleShutdown),
        ServiceWaitEvent::worker_completion);
}

TEST(SettingsServiceWaitTest, ShutdownWakeTakesPriorityOverWorkerCompletion) {
    const auto bus = descriptor(3, POLLIN);
    const auto worker = descriptor(4, POLLIN, POLLIN);
    const auto shutdown = descriptor(5, POLLIN, POLLIN);
    EXPECT_EQ(classifyServiceWait(2, 0, bus.events, bus, worker, shutdown),
              ServiceWaitEvent::shutdown);
}

TEST(SettingsServiceWaitTest, RetriesEintrAndFailsClosedForWaitOrDescriptorErrors) {
    const auto bus = descriptor(3, POLLIN);
    const auto worker = descriptor(4, POLLIN);
    const auto shutdown = descriptor(5, POLLIN);
    EXPECT_EQ(classifyServiceWait(-1, EINTR, bus.events, bus, worker, shutdown),
              ServiceWaitEvent::continue_processing);
    EXPECT_EQ(classifyServiceWait(-1, EIO, bus.events, bus, worker, shutdown),
              ServiceWaitEvent::disconnected);
    EXPECT_EQ(
        classifyServiceWait(1, 0, bus.events, descriptor(3, POLLIN, POLLHUP), worker, shutdown),
        ServiceWaitEvent::disconnected);
    EXPECT_EQ(classifyServiceWait(1, 0, bus.events, bus, descriptor(4, POLLIN, POLLNVAL), shutdown),
              ServiceWaitEvent::disconnected);
    EXPECT_EQ(classifyServiceWait(1, 0, bus.events, bus, worker, descriptor(5, POLLIN, POLLERR)),
              ServiceWaitEvent::disconnected);
    EXPECT_EQ(classifyServiceWait(1, 0, bus.events, bus, descriptor(4, POLLIN, POLLPRI), shutdown),
              ServiceWaitEvent::disconnected);
}

TEST(SettingsServiceWaitTest, SignalSafeNotificationWakesAndDrainsTheDescriptor) {
    auto wake = ServiceShutdownWake::create();
    ASSERT_TRUE(wake);

    notifyServiceShutdown(wake.value()->fileDescriptor());
    pollfd wakeDescriptor{wake.value()->fileDescriptor(), POLLIN, 0};
    ASSERT_EQ(::poll(&wakeDescriptor, 1U, 100), 1);
    EXPECT_EQ(wakeDescriptor.revents, POLLIN);

    wake.value()->drain();
    wakeDescriptor.revents = 0;
    EXPECT_EQ(::poll(&wakeDescriptor, 1U, 0), 0);
}

} // namespace
} // namespace prismdrake::settingsd
