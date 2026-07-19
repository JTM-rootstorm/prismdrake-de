#include "TaskIoWorker.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

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

    foundation::Result<x11::TaskModelObservation> refresh(std::span<const x11::WindowId>) override {
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

    foundation::Result<TaskEventRefreshPlan> drainEvents() override {
        char byte = 0;
        (void)read(state_->workerDescriptor, &byte, sizeof(byte));
        return foundation::Result<TaskEventRefreshPlan>::success({});
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
    const auto started = std::chrono::steady_clock::now();
    worker.value().reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{100});
}

} // namespace
} // namespace prismdrake::shell::taskcontroller
