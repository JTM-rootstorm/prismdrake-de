#include "TaskIoWorker.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace prismdrake::shell::taskcontroller {
namespace {

void ensureApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argc = 1;
    static char executable[] = "task-io-worker-test";
    static char *argv[] = {executable, nullptr};
    static QCoreApplication application(argc, argv);
    (void)application;
}

struct BlockingState final {
    std::mutex mutex;
    std::condition_variable enteredCondition;
    bool entered{false};
    bool released{false};
    std::thread::id workerThread;
    int workerDescriptor{-1};
    int releaseDescriptor{-1};

    ~BlockingState() {
        if (workerDescriptor >= 0) {
            (void)close(workerDescriptor);
        }
        if (releaseDescriptor >= 0) {
            (void)close(releaseDescriptor);
        }
    }
};

class BlockingOperations final : public TaskIoOperations {
  public:
    explicit BlockingOperations(std::shared_ptr<BlockingState> state) : state_(std::move(state)) {}

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }

    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->entered = true;
            state_->workerThread = std::this_thread::get_id();
        }
        state_->enteredCondition.notify_all();
        {
            std::unique_lock lock{state_->mutex};
            state_->enteredCondition.wait(lock, [&] { return state_->released; });
        }
        return foundation::Result<x11::TaskModelObservation>::failure(
            {foundation::ErrorCode::not_found, "The injected refresh completed.",
             "Finish the worker test."});
    }

    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        return foundation::Result<void>::failure({foundation::ErrorCode::unsupported,
                                                  "The injected request path is unused.",
                                                  "Use the refresh path in this test."});
    }

    [[nodiscard]] int eventFileDescriptor() const noexcept override {
        return state_->workerDescriptor;
    }

    foundation::Result<TaskEventDrain> drainEvents() override {
        char byte = 0;
        (void)recv(state_->workerDescriptor, &byte, sizeof(byte), MSG_DONTWAIT);
        return foundation::Result<TaskEventDrain>::success({{}, false});
    }

    void cancelWaits() noexcept override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->released = true;
        }
        state_->enteredCondition.notify_all();
    }

  private:
    std::shared_ptr<BlockingState> state_;
};

[[nodiscard]] x11::WindowId testWindow() { return x11::WindowId::fromProtocol(42U).value(); }

[[nodiscard]] x11::WindowIncarnationId testIncarnation() {
    return x11::WindowIncarnationId::fromObserved(1U).value();
}

[[nodiscard]] x11::TaskModelObservation testObservation() {
    auto list = x11::buildEwmhTaskListSnapshot(x11::EwmhTaskListObservation{
        std::vector<std::uint32_t>{testWindow().value()}, std::nullopt, std::nullopt});
    EXPECT_TRUE(list);
    x11::WindowMetadata metadata{
        testWindow(),
        "worker fixture",
        {x11::ApplicationIdentitySource::wmClass, std::nullopt, "Fixture", "fixture.desktop"},
        x11::WindowType::normal,
        true,
        {},
        0U,
        false,
        false,
        false,
        false,
        false,
        std::nullopt,
        {}};
    return {std::move(list).value(),
            {{testWindow(), testIncarnation(), std::move(metadata), "application-x-executable"}}};
}

class CountingOperations final : public TaskIoOperations {
  public:
    CountingOperations(int descriptor, std::shared_ptr<std::atomic<int>> count)
        : descriptor_(descriptor), count_(std::move(count)) {}
    ~CountingOperations() override { (void)close(descriptor_); }

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }

    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        return foundation::Result<x11::TaskModelObservation>::success(testObservation());
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        count_->fetch_add(1, std::memory_order_relaxed);
        return foundation::Result<void>::success();
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return descriptor_; }
    foundation::Result<TaskEventDrain> drainEvents() override {
        return foundation::Result<TaskEventDrain>::success({{}, false});
    }
    void cancelWaits() noexcept override { (void)shutdown(descriptor_, SHUT_RDWR); }

  private:
    int descriptor_;
    std::shared_ptr<std::atomic<int>> count_;
};

struct RefreshRecordingState final {
    std::atomic_bool invalidateAll{false};
    std::atomic_size_t invalidationCount{0U};
    int descriptor{-1};

    ~RefreshRecordingState() {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
    }
};

class RefreshRecordingOperations final : public TaskIoOperations {
  public:
    explicit RefreshRecordingOperations(std::shared_ptr<RefreshRecordingState> state)
        : state_(std::move(state)) {}

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }

    foundation::Result<x11::TaskModelObservation>
    refresh(std::span<const x11::WindowId> invalidations, bool invalidateAll) override {
        state_->invalidationCount.store(invalidations.size(), std::memory_order_relaxed);
        state_->invalidateAll.store(invalidateAll, std::memory_order_relaxed);
        return foundation::Result<x11::TaskModelObservation>::success(testObservation());
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        return foundation::Result<void>::success();
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return state_->descriptor; }
    foundation::Result<TaskEventDrain> drainEvents() override {
        return foundation::Result<TaskEventDrain>::success({{}, false});
    }
    void cancelWaits() noexcept override { (void)shutdown(state_->descriptor, SHUT_RDWR); }

  private:
    std::shared_ptr<RefreshRecordingState> state_;
};

struct EventStormState final {
    std::atomic<int> drains{0};
    int workerDescriptor{-1};
    int writerDescriptor{-1};

    ~EventStormState() {
        if (workerDescriptor >= 0) {
            (void)close(workerDescriptor);
        }
        if (writerDescriptor >= 0) {
            (void)close(writerDescriptor);
        }
    }
};

class EventStormOperations final : public TaskIoOperations {
  public:
    explicit EventStormOperations(std::shared_ptr<EventStormState> state)
        : state_(std::move(state)) {}

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }

    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        return foundation::Result<x11::TaskModelObservation>::success(testObservation());
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        return foundation::Result<void>::success();
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override {
        return state_->workerDescriptor;
    }
    foundation::Result<TaskEventDrain> drainEvents() override {
        char byte = 0;
        if (recv(state_->workerDescriptor, &byte, sizeof(byte), MSG_DONTWAIT) !=
            static_cast<ssize_t>(sizeof(byte))) {
            return foundation::Result<TaskEventDrain>::success({{}, false});
        }
        const auto index = state_->drains.fetch_add(1, std::memory_order_relaxed) + 1;
        auto window = x11::WindowId::fromProtocol(static_cast<std::uint32_t>(index));
        if (!window) {
            return foundation::Result<TaskEventDrain>::failure(window.error());
        }
        return foundation::Result<TaskEventDrain>::success(
            {{true, false, {window.value()}}, false});
    }
    void cancelWaits() noexcept override { (void)shutdown(state_->workerDescriptor, SHUT_RDWR); }

  private:
    std::shared_ptr<EventStormState> state_;
};

struct BlockingSetupState final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool cancelled{false};
};

class BlockingSetupOperations final : public TaskIoOperations {
  public:
    explicit BlockingSetupOperations(std::shared_ptr<BlockingSetupState> state)
        : state_(std::move(state)) {}

    foundation::Result<void> initialize() override {
        std::unique_lock lock{state_->mutex};
        state_->entered = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return state_->cancelled; });
        return foundation::Result<void>::failure({foundation::ErrorCode::cancelled,
                                                  "The injected setup was cancelled.",
                                                  "Finish the worker teardown test."});
    }
    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        return foundation::Result<x11::TaskModelObservation>::failure(
            {foundation::ErrorCode::cancelled, "The injected setup did not complete.",
             "Finish the worker teardown test."});
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        return foundation::Result<void>::failure({foundation::ErrorCode::cancelled,
                                                  "The injected setup did not complete.",
                                                  "Finish the worker teardown test."});
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return -1; }
    foundation::Result<TaskEventDrain> drainEvents() override {
        return foundation::Result<TaskEventDrain>::success({{}, false});
    }
    void cancelWaits() noexcept override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->cancelled = true;
        }
        state_->changed.notify_all();
    }

  private:
    std::shared_ptr<BlockingSetupState> state_;
};

struct BufferedFollowUpState final {
    std::atomic<int> drains{0};
    int workerDescriptor{-1};
    int writerDescriptor{-1};

    ~BufferedFollowUpState() {
        if (workerDescriptor >= 0) {
            (void)close(workerDescriptor);
        }
        if (writerDescriptor >= 0) {
            (void)close(writerDescriptor);
        }
    }
};

class BufferedFollowUpOperations final : public TaskIoOperations {
  public:
    explicit BufferedFollowUpOperations(std::shared_ptr<BufferedFollowUpState> state)
        : state_(std::move(state)) {}

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }

    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        return foundation::Result<x11::TaskModelObservation>::success(testObservation());
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        return foundation::Result<void>::success();
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override {
        return state_->workerDescriptor;
    }
    foundation::Result<TaskEventDrain> drainEvents() override {
        char byte = 0;
        if (state_->drains.load(std::memory_order_relaxed) == 0 &&
            recv(state_->workerDescriptor, &byte, sizeof(byte), MSG_DONTWAIT) !=
                static_cast<ssize_t>(sizeof(byte))) {
            return foundation::Result<TaskEventDrain>::success({{}, false});
        }
        const int drain = state_->drains.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drain == 1) {
            return foundation::Result<TaskEventDrain>::success({{}, true});
        }
        auto window = x11::WindowId::fromProtocol(77U);
        if (!window) {
            return foundation::Result<TaskEventDrain>::failure(window.error());
        }
        return foundation::Result<TaskEventDrain>::success(
            {{true, false, {window.value()}}, false});
    }
    void cancelWaits() noexcept override { (void)shutdown(state_->workerDescriptor, SHUT_RDWR); }

  private:
    std::shared_ptr<BufferedFollowUpState> state_;
};

struct RequestBufferedEventState final {
    std::atomic_bool eventBuffered{false};
    std::atomic_bool requestHandled{false};
};

class RequestBufferedEventOperations final : public TaskIoOperations {
  public:
    explicit RequestBufferedEventOperations(std::shared_ptr<RequestBufferedEventState> state)
        : state_(std::move(state)) {}

    foundation::Result<void> initialize() override { return foundation::Result<void>::success(); }
    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>,
                                                          bool) override {
        return foundation::Result<x11::TaskModelObservation>::success(testObservation());
    }
    foundation::Result<void> request(const x11::TaskRequestState &,
                                     std::optional<x11::WindowId>) override {
        state_->eventBuffered.store(true, std::memory_order_release);
        state_->requestHandled.store(true, std::memory_order_release);
        return foundation::Result<void>::success();
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return -1; }
    foundation::Result<TaskEventDrain> drainEvents() override {
        if (!state_->eventBuffered.exchange(false, std::memory_order_acq_rel)) {
            return foundation::Result<TaskEventDrain>::success({{}, false});
        }
        auto window = x11::WindowId::fromProtocol(88U);
        if (!window) {
            return foundation::Result<TaskEventDrain>::failure(window.error());
        }
        return foundation::Result<TaskEventDrain>::success(
            {{true, false, {window.value()}}, false});
    }
    void cancelWaits() noexcept override {}

  private:
    std::shared_ptr<RequestBufferedEventState> state_;
};

[[nodiscard]] std::shared_ptr<BlockingState> blockingState() {
    int descriptors[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0) {
        return {};
    }
    auto state = std::make_shared<BlockingState>();
    state->workerDescriptor = descriptors[0];
    state->releaseDescriptor = descriptors[1];
    return state;
}

[[nodiscard]] TaskIoWorker::OperationsFactory factory(std::shared_ptr<BlockingState> state) {
    return [state = std::move(state)] {
        return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
            std::make_unique<BlockingOperations>(state));
    };
}

TEST(TaskIoWorkerTest, BlockingRefreshLeavesOwnerHeartbeatResponsive) {
    ensureApplication();
    auto state = blockingState();
    ASSERT_TRUE(state);
    QObject owner;
    bool completed = false;
    auto worker = TaskIoWorker::create(
        owner, factory(state), [&completed](std::uint64_t, auto) { completed = true; },
        [](auto, auto) {}, [](auto) {});
    ASSERT_TRUE(worker);

    QElapsedTimer elapsed;
    elapsed.start();
    bool heartbeat = false;
    QEventLoop heartbeatLoop;
    QTimer::singleShot(0, &heartbeatLoop, [&] {
        heartbeat = true;
        heartbeatLoop.quit();
    });
    ASSERT_TRUE(worker.value()->submitRefresh(1U, {}));
    heartbeatLoop.exec();
    EXPECT_TRUE(heartbeat);
    EXPECT_LT(elapsed.elapsed(), 100);
    EXPECT_FALSE(completed);

    {
        std::unique_lock lock{state->mutex};
        ASSERT_TRUE(state->enteredCondition.wait_for(lock, std::chrono::seconds{1},
                                                     [&] { return state->entered; }));
        EXPECT_NE(state->workerThread, std::this_thread::get_id());
    }
    {
        std::scoped_lock lock{state->mutex};
        state->released = true;
    }
    state->enteredCondition.notify_all();
    QEventLoop completionLoop;
    QTimer completionPoll;
    completionPoll.setInterval(1);
    QObject::connect(&completionPoll, &QTimer::timeout, &completionLoop, [&] {
        if (completed) {
            completionLoop.quit();
        }
    });
    QTimer::singleShot(1000, &completionLoop, &QEventLoop::quit);
    completionPoll.start();
    completionLoop.exec();
    EXPECT_TRUE(completed);
}

TEST(TaskIoWorkerTest, TeardownInterruptsAnUnresponsiveX11Wait) {
    ensureApplication();
    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 32; ++iteration) {
        auto state = blockingState();
        ASSERT_TRUE(state);
        QObject owner;
        auto worker = TaskIoWorker::create(
            owner, factory(state), [](auto, auto) {}, [](auto, auto) {}, [](auto) {});
        ASSERT_TRUE(worker);
        ASSERT_TRUE(worker.value()->submitRefresh(1U, {}));
        {
            std::unique_lock lock{state->mutex};
            ASSERT_TRUE(state->enteredCondition.wait_for(lock, std::chrono::seconds{1},
                                                         [&] { return state->entered; }));
        }
        worker.value().reset();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{1});
}

TEST(TaskIoWorkerTest, TeardownInterruptsReplyBearingOperationsSetup) {
    ensureApplication();
    auto state = std::make_shared<BlockingSetupState>();
    QObject owner;
    auto worker = TaskIoWorker::create(
        owner,
        [state] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<BlockingSetupOperations>(state));
        },
        [](auto, auto) {}, [](auto, auto) {}, [](auto) {});
    ASSERT_TRUE(worker);
    {
        std::unique_lock lock{state->mutex};
        ASSERT_TRUE(
            state->changed.wait_for(lock, std::chrono::seconds{1}, [&] { return state->entered; }));
    }

    const auto started = std::chrono::steady_clock::now();
    worker.value().reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{100});
}

TEST(TaskIoWorkerTest, OversizedInvalidationsResetTheAuthoritativeSource) {
    ensureApplication();
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    auto state = std::make_shared<RefreshRecordingState>();
    state->descriptor = descriptors[0];
    QObject owner;
    bool completed = false;
    auto worker = TaskIoWorker::create(
        owner,
        [state] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<RefreshRecordingOperations>(state));
        },
        [&completed](auto, auto result) {
            EXPECT_TRUE(result);
            completed = true;
        },
        [](auto, auto) {}, [](auto) {});
    ASSERT_TRUE(worker);
    std::vector<x11::WindowId> invalidations;
    for (std::uint32_t value = 1U; value <= x11::maximumTaskWindows + 1U; ++value) {
        invalidations.push_back(x11::WindowId::fromProtocol(value).value());
    }
    ASSERT_TRUE(worker.value()->submitRefresh(1U, std::move(invalidations)));

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (completed) {
            loop.quit();
        }
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_TRUE(state->invalidateAll.load(std::memory_order_relaxed));
    EXPECT_EQ(state->invalidationCount.load(std::memory_order_relaxed), 0U);
    (void)close(descriptors[1]);
}

TEST(TaskIoWorkerTest, DrainsMultipleRequestsAfterOneAggregatedWake) {
    ensureApplication();
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    auto count = std::make_shared<std::atomic<int>>(0);
    QObject owner;
    int completions = 0;
    auto worker = TaskIoWorker::create(
        owner,
        [descriptor = descriptors[0], count] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<CountingOperations>(descriptor, count));
        },
        [](auto, auto) {}, [&completions](auto, auto) { ++completions; }, [](auto) {});
    ASSERT_TRUE(worker);

    x11::TaskModel model;
    auto snapshot = model.publish(testObservation());
    ASSERT_TRUE(snapshot);
    const auto lifetime = snapshot.value()->tasks().front().lifetime();
    auto activate = x11::TaskRequestState::issue(*snapshot.value(), lifetime,
                                                 x11::TaskRequestAction::activate, 8U);
    auto closeRequest = x11::TaskRequestState::issue(*snapshot.value(), lifetime,
                                                     x11::TaskRequestAction::close, 8U);
    ASSERT_TRUE(activate);
    ASSERT_TRUE(closeRequest);
    ASSERT_TRUE(worker.value()->submitRefresh(1U, {}));
    ASSERT_TRUE(worker.value()->submitRequest(1U, activate.value(), std::nullopt));
    ASSERT_TRUE(worker.value()->submitRequest(1U, closeRequest.value(), std::nullopt));

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (completions == 2) {
            loop.quit();
        }
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    EXPECT_EQ(completions, 2);
    EXPECT_EQ(count->load(std::memory_order_relaxed), 2);
    (void)close(descriptors[1]);
}

TEST(TaskIoWorkerTest, EventStormQueuesOneBoundedFullRefreshForTheOwner) {
    ensureApplication();
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    auto state = std::make_shared<EventStormState>();
    state->workerDescriptor = descriptors[0];
    state->writerDescriptor = descriptors[1];
    QObject owner;
    std::vector<TaskEventRefreshPlan> deliveries;
    auto worker = TaskIoWorker::create(
        owner,
        [state] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<EventStormOperations>(state));
        },
        [](auto, auto) {}, [](auto, auto) {},
        [&deliveries](auto result) {
            ASSERT_TRUE(result);
            deliveries.push_back(std::move(result).value());
        });
    ASSERT_TRUE(worker);

    constexpr int eventCount = static_cast<int>(x11::maximumTaskWindows) + 32;
    std::array<char, eventCount> events{};
    ASSERT_EQ(write(state->writerDescriptor, events.data(), events.size()),
              static_cast<ssize_t>(events.size()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (state->drains.load(std::memory_order_relaxed) < eventCount &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(state->drains.load(std::memory_order_relaxed), eventCount);
    EXPECT_TRUE(deliveries.empty());

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!deliveries.empty()) {
            loop.quit();
        }
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();

    ASSERT_EQ(deliveries.size(), 1U);
    EXPECT_TRUE(deliveries.front().refreshRequired);
    EXPECT_TRUE(deliveries.front().invalidateAllClients);
    EXPECT_TRUE(deliveries.front().invalidatedWindows.empty());
}

TEST(TaskIoWorkerTest, ExaminationLimitDrainsBufferedFollowUpWithoutAnotherDescriptorWake) {
    ensureApplication();
    int descriptors[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors), 0);
    auto state = std::make_shared<BufferedFollowUpState>();
    state->workerDescriptor = descriptors[0];
    state->writerDescriptor = descriptors[1];
    QObject owner;
    std::vector<TaskEventRefreshPlan> deliveries;
    auto worker = TaskIoWorker::create(
        owner,
        [state] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<BufferedFollowUpOperations>(state));
        },
        [](auto, auto) {}, [](auto, auto) {},
        [&deliveries](auto result) {
            ASSERT_TRUE(result);
            deliveries.push_back(std::move(result).value());
        });
    ASSERT_TRUE(worker);
    const char signal = 1;
    ASSERT_EQ(write(state->writerDescriptor, &signal, sizeof(signal)),
              static_cast<ssize_t>(sizeof(signal)));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (state->drains.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(state->drains.load(std::memory_order_relaxed), 2);

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!deliveries.empty()) {
            loop.quit();
        }
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();

    ASSERT_EQ(deliveries.size(), 1U);
    EXPECT_TRUE(deliveries.front().refreshRequired);
    ASSERT_EQ(deliveries.front().invalidatedWindows.size(), 1U);
    EXPECT_EQ(deliveries.front().invalidatedWindows.front().value(), 77U);
}

TEST(TaskIoWorkerTest, CheckedRequestDrainsBufferedEventWithoutDescriptorWake) {
    ensureApplication();
    auto state = std::make_shared<RequestBufferedEventState>();
    QObject owner;
    bool refreshCompleted = false;
    std::vector<TaskEventRefreshPlan> deliveries;
    auto worker = TaskIoWorker::create(
        owner,
        [state] {
            return foundation::Result<std::unique_ptr<TaskIoOperations>>::success(
                std::make_unique<RequestBufferedEventOperations>(state));
        },
        [&refreshCompleted](auto, auto result) {
            ASSERT_TRUE(result);
            refreshCompleted = true;
        },
        [](auto, auto) {},
        [&deliveries](auto result) {
            ASSERT_TRUE(result);
            if (result.value().refreshRequired) {
                deliveries.push_back(std::move(result).value());
            }
        });
    ASSERT_TRUE(worker);
    ASSERT_TRUE(worker.value()->submitRefresh(1U, {}));

    QEventLoop refreshLoop;
    QTimer refreshPoll;
    refreshPoll.setInterval(1);
    QObject::connect(&refreshPoll, &QTimer::timeout, &refreshLoop, [&] {
        if (refreshCompleted) {
            refreshLoop.quit();
        }
    });
    QTimer::singleShot(1000, &refreshLoop, &QEventLoop::quit);
    refreshPoll.start();
    refreshLoop.exec();
    ASSERT_TRUE(refreshCompleted);

    x11::TaskModel model;
    auto snapshot = model.publish(testObservation());
    ASSERT_TRUE(snapshot);
    auto request = x11::TaskRequestState::issue(*snapshot.value(),
                                                snapshot.value()->tasks().front().lifetime(),
                                                x11::TaskRequestAction::activate, 1U);
    ASSERT_TRUE(request);
    ASSERT_TRUE(worker.value()->submitRequest(1U, request.value(), std::nullopt));

    QEventLoop requestLoop;
    QTimer requestPoll;
    requestPoll.setInterval(1);
    QObject::connect(&requestPoll, &QTimer::timeout, &requestLoop, [&] {
        if (state->requestHandled.load(std::memory_order_acquire) && !deliveries.empty()) {
            requestLoop.quit();
        }
    });
    QTimer::singleShot(1000, &requestLoop, &QEventLoop::quit);
    requestPoll.start();
    requestLoop.exec();

    EXPECT_TRUE(state->requestHandled.load(std::memory_order_acquire));
    ASSERT_EQ(deliveries.size(), 1U);
    ASSERT_EQ(deliveries.front().invalidatedWindows.size(), 1U);
    EXPECT_EQ(deliveries.front().invalidatedWindows.front().value(), 88U);
}

} // namespace
} // namespace prismdrake::shell::taskcontroller
