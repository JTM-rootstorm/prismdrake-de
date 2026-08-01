#include "X11PropertyWorker.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <span>

namespace prismdrake::shell::window {
namespace {

[[nodiscard]] x11::WindowId window(std::uint32_t value) {
    return x11::WindowId::fromProtocol(value).value();
}

TEST(X11PropertyWorkerTest, GenericTransactionsRejectInitialTaskPolicyOperations) {
    QObject owner;
    auto worker = X11PropertyWorker::create(
        owner, ":test", [](std::uint64_t, foundation::Result<void>) {},
        [](std::stop_token, std::span<const X11PropertyOperation>) {
            return foundation::Result<void>::success();
        });
    ASSERT_TRUE(worker);

    EXPECT_FALSE(worker.value()->submit(
        1U, {{X11PropertyOperationKind::publishInitialSkipTaskbar, window(10U), std::nullopt}}));
    EXPECT_FALSE(worker.value()->submit(
        2U, {{X11PropertyOperationKind::publishDesktop, window(11U), std::nullopt},
             {X11PropertyOperationKind::publishInitialSkipTaskbar, window(12U), std::nullopt}}));
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
                return operation.kind == X11PropertyOperationKind::publishInitialSkipTaskbar &&
                       !operation.dockReservation;
            });
            return valid ? foundation::Result<void>::success()
                         : foundation::Result<void>::failure(
                               {foundation::ErrorCode::validation_error,
                                "The staged task-policy batch is invalid.",
                                "Discard the unpublished test windows."});
        });
    ASSERT_TRUE(worker);

    EXPECT_FALSE(worker.value()->submitInitialSkipTaskbar(0U, {window(20U)}));
    EXPECT_FALSE(worker.value()->submitInitialSkipTaskbar(1U, {}));
    EXPECT_FALSE(worker.value()->submitInitialSkipTaskbar(1U, {window(20U), window(20U)}));
    std::vector<x11::WindowId> oversized;
    oversized.reserve(129U);
    for (std::uint32_t identifier = 100U; identifier < 229U; ++identifier) {
        oversized.push_back(window(identifier));
    }
    EXPECT_FALSE(worker.value()->submitInitialSkipTaskbar(2U, std::move(oversized)));
    ASSERT_TRUE(worker.value()->submitInitialSkipTaskbar(
        3U, {window(20U), window(21U), window(22U), window(23U), window(24U)}));
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_TRUE(completed);
    EXPECT_EQ(observedOperations, 5U);
}

} // namespace
} // namespace prismdrake::shell::window

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
