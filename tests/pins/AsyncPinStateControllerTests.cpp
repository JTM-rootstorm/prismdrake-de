#include "AsyncPinStateController.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::panel {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] launcher::DesktopFileId pin(std::string value) {
    auto result = launcher::deriveDesktopFileId(value);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

class BlockingBackend final : public PinStatePersistenceBackend {
  public:
    explicit BlockingBackend(std::shared_ptr<std::atomic_bool> stopObserved = {})
        : current_(std::make_shared<const PinStateSnapshot>(
              PinStateSnapshot{1U, {}, PinStateLoadStatus::missing})),
          stop_observed_(std::move(stopObserved)) {}

    void requestStop() noexcept override {
        const std::scoped_lock lock{mutex_};
        stop_requested_ = true;
        replace_released_ = true;
        if (stop_observed_) {
            *stop_observed_ = true;
        }
        condition_.notify_all();
    }

    foundation::Result<std::shared_ptr<const PinStateSnapshot>>
    open(const std::filesystem::path &) override {
        open_thread_ = std::this_thread::get_id();
        return foundation::Result<std::shared_ptr<const PinStateSnapshot>>::success(current_);
    }

    foundation::Result<std::shared_ptr<const PinStateSnapshot>>
    replace(std::vector<launcher::DesktopFileId> pins) override {
        std::unique_lock lock{mutex_};
        replace_thread_ = std::this_thread::get_id();
        replace_entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return replace_released_; });
        replace_released_ = false;
        replace_entered_ = false;
        if (stop_requested_) {
            return foundation::Result<std::shared_ptr<const PinStateSnapshot>>::failure(
                {foundation::ErrorCode::cancelled, "The injected pin writer stopped.",
                 "Discard the stopped persistence request."});
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"The injected pin writer threw."};
        }
        if (fail_next_) {
            fail_next_ = false;
            return foundation::Result<std::shared_ptr<const PinStateSnapshot>>::failure(
                {foundation::ErrorCode::io_error, "The injected pin write failed.",
                 "Retain the prior complete pin snapshot."});
        }
        current_ = std::make_shared<const PinStateSnapshot>(PinStateSnapshot{
            current_->generation + 1U, std::move(pins), PinStateLoadStatus::loaded});
        return foundation::Result<std::shared_ptr<const PinStateSnapshot>>::success(current_);
    }

    [[nodiscard]] bool waitForReplace() {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, 2s, [this] { return replace_entered_; });
    }

    void releaseReplace(bool fail = false, bool throwException = false) {
        const std::scoped_lock lock{mutex_};
        fail_next_ = fail;
        throw_next_ = throwException;
        replace_released_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::thread::id openThread() const noexcept { return open_thread_; }
    [[nodiscard]] std::thread::id replaceThread() const noexcept { return replace_thread_; }

  private:
    std::shared_ptr<const PinStateSnapshot> current_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool replace_entered_{false};
    bool replace_released_{false};
    bool fail_next_{false};
    bool throw_next_{false};
    bool stop_requested_{false};
    std::shared_ptr<std::atomic_bool> stop_observed_;
    std::thread::id open_thread_;
    std::thread::id replace_thread_;
};

struct ControllerFixture final {
    std::unique_ptr<AsyncPinStateController> controller;
    BlockingBackend *backend;
};

[[nodiscard]] ControllerFixture startedController() {
    auto backend = std::make_unique<BlockingBackend>();
    auto *backendPointer = backend.get();
    auto created = AsyncPinStateController::create(
        "/tmp/prismdrake-async-pin-state-tests/prismdrake", std::move(backend));
    EXPECT_TRUE(created);
    auto controller = std::move(created).value();
    QSignalSpy loaded{controller.get(), &AsyncPinStateController::snapshotChanged};
    EXPECT_TRUE(controller->start());
    EXPECT_TRUE(loaded.wait(2000));
    EXPECT_EQ(loaded.count(), 1);
    EXPECT_TRUE(controller->current());
    return {std::move(controller), backendPointer};
}

TEST(AsyncPinStateControllerTest, InitialLoadAndBlockedWriteNeverRunOnOwnerThread) {
    const auto ownerThread = std::this_thread::get_id();
    auto fixture = startedController();
    ASSERT_NE(fixture.backend->openThread(), ownerThread);

    QSignalSpy published{fixture.controller.get(), &AsyncPinStateController::snapshotChanged};
    bool heartbeat = false;
    ASSERT_TRUE(fixture.controller->replace({pin("blocked.desktop")}));
    ASSERT_TRUE(fixture.backend->waitForReplace());
    ASSERT_NE(fixture.backend->replaceThread(), ownerThread);
    QTimer::singleShot(0, fixture.controller.get(), [&heartbeat] { heartbeat = true; });
    QTRY_VERIFY_WITH_TIMEOUT(heartbeat, 1000);
    EXPECT_EQ(fixture.controller->current()->generation, 1U);
    EXPECT_EQ(published.count(), 0);

    fixture.backend->releaseReplace();
    ASSERT_TRUE(published.wait(2000));
    EXPECT_EQ(fixture.controller->current()->generation, 2U);
}

TEST(AsyncPinStateControllerTest, FailedPersistenceRetainsPriorCompleteGeneration) {
    auto fixture = startedController();
    const auto prior = fixture.controller->current();
    QSignalSpy published{fixture.controller.get(), &AsyncPinStateController::snapshotChanged};
    QSignalSpy failed{fixture.controller.get(), &AsyncPinStateController::persistenceFailed};

    ASSERT_TRUE(fixture.controller->replace({pin("failure.desktop")}));
    ASSERT_TRUE(fixture.backend->waitForReplace());
    fixture.backend->releaseReplace(true);
    ASSERT_TRUE(failed.wait(2000));

    EXPECT_EQ(published.count(), 0);
    EXPECT_EQ(fixture.controller->current(), prior);
    EXPECT_EQ(fixture.controller->current()->generation, 1U);
    ASSERT_TRUE(fixture.controller->lastError());
    EXPECT_EQ(fixture.controller->lastError()->code, foundation::ErrorCode::io_error);
    EXPECT_FALSE(fixture.controller->busy());
}

TEST(AsyncPinStateControllerTest, SuccessfulPersistencePublishesExactlyOnceOnOwnerThread) {
    auto fixture = startedController();
    int publications = 0;
    bool publishedOnOwnerThread = false;
    QObject::connect(fixture.controller.get(), &AsyncPinStateController::snapshotChanged,
                     fixture.controller.get(), [&] {
                         ++publications;
                         publishedOnOwnerThread =
                             QThread::currentThread() == fixture.controller->thread();
                     });

    ASSERT_TRUE(fixture.controller->replace({pin("success.desktop")}));
    ASSERT_FALSE(fixture.controller->replace({pin("overlap.desktop")}));
    ASSERT_TRUE(fixture.backend->waitForReplace());
    fixture.backend->releaseReplace();
    QTRY_COMPARE_WITH_TIMEOUT(publications, 1, 2000);
    QTest::qWait(50);

    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(publishedOnOwnerThread);
    ASSERT_EQ(fixture.controller->current()->pins.size(), 1U);
    EXPECT_EQ(fixture.controller->current()->pins.front().value(), "success.desktop");
    EXPECT_FALSE(fixture.controller->busy());
}

TEST(AsyncPinStateControllerTest, ThrowingBackendFailsClosedAndRetainsPriorGeneration) {
    auto fixture = startedController();
    const auto prior = fixture.controller->current();
    QSignalSpy failed{fixture.controller.get(), &AsyncPinStateController::persistenceFailed};

    ASSERT_TRUE(fixture.controller->replace({pin("throw.desktop")}));
    ASSERT_TRUE(fixture.backend->waitForReplace());
    fixture.backend->releaseReplace(false, true);
    ASSERT_TRUE(failed.wait(2000));

    EXPECT_EQ(fixture.controller->current(), prior);
    ASSERT_TRUE(fixture.controller->lastError());
    EXPECT_EQ(fixture.controller->lastError()->code, foundation::ErrorCode::io_error);
    EXPECT_EQ(fixture.controller->lastError()->message,
              "The pin-state persistence backend failed unexpectedly.");
}

TEST(AsyncPinStateControllerTest, StopInterruptsAnInjectedBlockedBackend) {
    auto stopObserved = std::make_shared<std::atomic_bool>(false);
    auto backend = std::make_unique<BlockingBackend>(stopObserved);
    auto *backendPointer = backend.get();
    auto created = AsyncPinStateController::create(
        "/tmp/prismdrake-async-pin-state-tests/prismdrake", std::move(backend));
    ASSERT_TRUE(created);
    auto controller = std::move(created).value();
    QSignalSpy loaded{controller.get(), &AsyncPinStateController::snapshotChanged};
    ASSERT_TRUE(controller->start());
    ASSERT_TRUE(loaded.wait(2000));
    ASSERT_TRUE(controller->replace({pin("stop.desktop")}));
    ASSERT_TRUE(backendPointer->waitForReplace());

    controller.reset();
    EXPECT_TRUE(*stopObserved);
}

} // namespace
} // namespace prismdrake::shell::panel

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
