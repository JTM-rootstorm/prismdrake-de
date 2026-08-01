#include "X11PropertyWorker.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <span>
#include <vector>

namespace prismdrake::shell::window {
namespace {

[[nodiscard]] x11::WindowId window(std::uint32_t value) {
    return x11::WindowId::fromProtocol(value).value();
}

TEST(X11PropertyWorkerTest, GenericTransactionsRejectUnmappedTaskPolicyOperations) {
    QObject owner;
    auto worker = X11PropertyWorker::create(
        owner, ":test", [](std::uint64_t, foundation::Result<void>) {},
        [](std::stop_token, std::span<const X11PropertyOperation>) {
            return foundation::Result<void>::success();
        });
    ASSERT_TRUE(worker);

    EXPECT_FALSE(worker.value()->submit(
        1U, {{X11PropertyOperationKind::publishUnmappedSkipTaskbar, window(10U), std::nullopt}}));
    EXPECT_FALSE(worker.value()->submit(
        2U, {{X11PropertyOperationKind::publishDesktop, window(11U), std::nullopt},
             {X11PropertyOperationKind::publishUnmappedSkipTaskbar, window(12U), std::nullopt}}));
}

TEST(X11PropertyWorkerTest, DedicatedTaskPolicyBatchIsBoundedDistinctAndDeliveredTogether) {
    QObject owner;
    QEventLoop loop;
    std::size_t observedOperations = 0U;
    bool completed = false;
    auto worker = X11PropertyWorker::create(
        owner, ":test",
        [&loop, &completed](std::uint64_t token, foundation::Result<void> result) {
            completed = token == 3U && result.hasValue();
            loop.quit();
        },
        [&observedOperations](std::stop_token, std::span<const X11PropertyOperation> operations) {
            observedOperations = operations.size();
            const bool valid = std::ranges::all_of(operations, [](const auto &operation) {
                return operation.kind == X11PropertyOperationKind::publishUnmappedSkipTaskbar &&
                       !operation.dockReservation;
            });
            return valid ? foundation::Result<void>::success()
                         : foundation::Result<void>::failure(
                               {foundation::ErrorCode::validation_error,
                                "The staged task-policy batch is invalid.",
                                "Discard the unmapped test windows."});
        });
    ASSERT_TRUE(worker);

    EXPECT_FALSE(worker.value()->submitUnmappedSkipTaskbar(0U, {window(20U)}));
    EXPECT_FALSE(worker.value()->submitUnmappedSkipTaskbar(1U, {}));
    EXPECT_FALSE(worker.value()->submitUnmappedSkipTaskbar(1U, {window(20U), window(20U)}));
    std::vector<x11::WindowId> oversized;
    oversized.reserve(129U);
    for (std::uint32_t identifier = 100U; identifier < 229U; ++identifier) {
        oversized.push_back(window(identifier));
    }
    EXPECT_FALSE(worker.value()->submitUnmappedSkipTaskbar(2U, std::move(oversized)));
    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(
        3U, {window(20U), window(21U), window(22U), window(23U), window(24U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(observedOperations, 5U);
}

TEST(X11PropertyWorkerTest, DedicatedTaskPolicySupportsSequentialUnmappedPublications) {
    QObject owner;
    QEventLoop loop;
    std::mutex observationsMutex;
    std::vector<std::uint64_t> completedTokens;
    std::vector<x11::WindowId::Value> publishedWindows;
    auto worker = X11PropertyWorker::create(
        owner, ":test",
        [&loop, &completedTokens](std::uint64_t token, foundation::Result<void> result) {
            EXPECT_TRUE(result);
            completedTokens.push_back(token);
            if (completedTokens.size() == 2U) {
                loop.quit();
            }
        },
        [&observationsMutex, &publishedWindows](std::stop_token,
                                                std::span<const X11PropertyOperation> operations) {
            if (operations.size() != 1U ||
                operations.front().kind != X11PropertyOperationKind::publishUnmappedSkipTaskbar ||
                operations.front().dockReservation) {
                return foundation::Result<void>::failure(
                    {foundation::ErrorCode::validation_error,
                     "The sequential task-policy operation is invalid.",
                     "Discard the unmapped test window."});
            }
            std::scoped_lock lock{observationsMutex};
            publishedWindows.push_back(operations.front().window.value());
            return foundation::Result<void>::success();
        });
    ASSERT_TRUE(worker);

    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(4U, {window(30U)}));
    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(5U, {window(31U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(completedTokens, (std::vector<std::uint64_t>{4U, 5U}));
    std::scoped_lock lock{observationsMutex};
    EXPECT_EQ(publishedWindows, (std::vector<x11::WindowId::Value>{30U, 31U}));
}

TEST(X11PropertyWorkerTest, RetriesTransientUnmappedPublicationFailuresUntilSuccess) {
    QObject owner;
    QEventLoop loop;
    std::atomic_size_t attempts{0U};
    bool completed = false;
    auto worker = X11PropertyWorker::create(
        owner, ":test",
        [&loop, &completed](std::uint64_t token, foundation::Result<void> result) {
            completed = token == 6U && result.hasValue();
            loop.quit();
        },
        [&attempts](std::stop_token, std::span<const X11PropertyOperation>) {
            if (attempts.fetch_add(1U) < 2U) {
                return foundation::Result<void>::failure(
                    {foundation::ErrorCode::invalid_argument,
                     "The unmapped test window is not observable yet.",
                     "Retry the checked publication within its bounded worker budget."});
            }
            return foundation::Result<void>::success();
        });
    ASSERT_TRUE(worker);

    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(6U, {window(40U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(attempts.load(), 3U);
}

TEST(X11PropertyWorkerTest, BoundsPersistentTransientUnmappedPublicationFailures) {
    QObject owner;
    QEventLoop loop;
    std::atomic_size_t attempts{0U};
    bool completed = false;
    auto worker = X11PropertyWorker::create(
        owner, ":test",
        [&loop, &completed](std::uint64_t token, foundation::Result<void> result) {
            completed =
                token == 7U && !result && result.error().code == foundation::ErrorCode::io_error;
            loop.quit();
        },
        [&attempts](std::stop_token, std::span<const X11PropertyOperation>) {
            attempts.fetch_add(1U);
            return foundation::Result<void>::failure(
                {foundation::ErrorCode::io_error, "The unmapped test window remains unavailable.",
                 "Fail after the bounded worker retry budget is exhausted."});
        });
    ASSERT_TRUE(worker);

    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(7U, {window(41U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(attempts.load(), 32U);
}

TEST(X11PropertyWorkerTest, DoesNotRetryNonTransientUnmappedPublicationFailures) {
    QObject owner;
    QEventLoop loop;
    std::atomic_size_t attempts{0U};
    bool completed = false;
    auto worker = X11PropertyWorker::create(
        owner, ":test",
        [&loop, &completed](std::uint64_t token, foundation::Result<void> result) {
            completed = token == 8U && !result &&
                        result.error().code == foundation::ErrorCode::validation_error;
            loop.quit();
        },
        [&attempts](std::stop_token, std::span<const X11PropertyOperation>) {
            attempts.fetch_add(1U);
            return foundation::Result<void>::failure(
                {foundation::ErrorCode::validation_error,
                 "The unmapped test window state is malformed.",
                 "Discard the target without retrying malformed state."});
        });
    ASSERT_TRUE(worker);

    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(8U, {window(42U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(attempts.load(), 1U);
}

TEST(X11PropertyWorkerTest, StopRequestInterruptsTransientUnmappedPublicationRetry) {
    QObject owner;
    QEventLoop loop;
    QTimer observer;
    std::atomic_size_t attempts{0U};
    auto worker = X11PropertyWorker::create(
        owner, ":test", [](std::uint64_t, foundation::Result<void>) {},
        [&attempts](std::stop_token, std::span<const X11PropertyOperation>) {
            attempts.fetch_add(1U);
            return foundation::Result<void>::failure(
                {foundation::ErrorCode::io_error, "The unmapped test window remains unavailable.",
                 "Cancel the bounded retry when the worker stops."});
        });
    ASSERT_TRUE(worker);

    QObject::connect(&observer, &QTimer::timeout, &loop, [&attempts, &loop] {
        if (attempts.load() > 0U) {
            loop.quit();
        }
    });
    observer.start(1);
    ASSERT_TRUE(worker.value()->submitUnmappedSkipTaskbar(9U, {window(43U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    ASSERT_GT(attempts.load(), 0U);
    worker.value().reset();
    EXPECT_LT(attempts.load(), 32U);
}

} // namespace
} // namespace prismdrake::shell::window

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
