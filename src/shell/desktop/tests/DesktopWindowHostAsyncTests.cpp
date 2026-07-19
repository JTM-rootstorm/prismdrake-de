#include "DesktopWindowHost.hpp"

#include "OutputSurfaceRegistry.hpp"
#include "OutputTopology.hpp"
#include "ScreenReconciliation.hpp"
#include "TestTopology.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::desktop {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using namespace std::chrono_literals;

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

[[nodiscard]] Result<void> injectedFailure() {
    return Result<void>::failure({ErrorCode::io_error, "Injected desktop publication failure.",
                                  "Retain the prior complete desktop generation."});
}

class TransactionGate final {
  public:
    [[nodiscard]] Result<void> run(std::stop_token stopToken,
                                   std::span<const window::X11PropertyOperation> operations) {
        {
            std::scoped_lock lock{mutex_};
            transactions_.emplace_back();
            for (const auto &operation : operations) {
                transactions_.back().push_back(operation.kind);
            }
        }
        if (std::ranges::all_of(operations, [](const auto &operation) {
                return operation.kind == window::X11PropertyOperationKind::removeDesktop;
            })) {
            return Result<void>::success();
        }
        std::unique_lock lock{mutex_};
        const auto sequence = ++started_;
        changed_.notify_all();
        changed_.wait(lock, stopToken, [&] { return outcomes_.size() >= sequence; });
        if (stopToken.stop_requested()) {
            return Result<void>::failure({ErrorCode::cancelled,
                                          "Injected desktop publication cancelled.",
                                          "Discard the incomplete desktop generation."});
        }
        return std::move(outcomes_[sequence - 1U]);
    }

    void release(Result<void> outcome) {
        {
            std::scoped_lock lock{mutex_};
            outcomes_.push_back(std::move(outcome));
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool started(std::size_t count) const {
        std::scoped_lock lock{mutex_};
        return started_ >= count;
    }

    [[nodiscard]] std::vector<window::X11PropertyOperationKind>
    transaction(std::size_t index) const {
        std::scoped_lock lock{mutex_};
        return index < transactions_.size() ? transactions_[index]
                                            : std::vector<window::X11PropertyOperationKind>{};
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Result<void>> outcomes_;
    std::vector<std::vector<window::X11PropertyOperationKind>> transactions_;
    std::size_t started_{0U};
};

class SameTopologyRegistry final {
  public:
    SameTopologyRegistry() : registry_(outputs::OutputSurfaceRegistry::create(48U).value()) {
        const auto root = x11::RootGeometry::create(1024U, 768U).value();
        const std::vector candidates{test::candidate(1U, 10U, "LEFT-1", 0, 512U, 768U),
                                     test::candidate(2U, 20U, "RIGHT-1", 512, 512U, 768U)};
        const auto topology =
            x11::buildOutputTopology({true, root, 2U, 2U, 1U, candidates, test::outputId(1U)})
                .value();
        const std::array screens{
            outputs::QtScreenObservation{"LEFT-1", {0, 0, 512U, 768U}, 1.0},
            outputs::QtScreenObservation{"RIGHT-1", {512, 0, 512U, 768U}, 1.0}};
        topology_ = outputs::reconcileScreens(1U, topology, screens).value();
    }

    [[nodiscard]] outputs::SurfaceRegistrySnapshot settings(std::uint64_t generation) {
        return registry_.observe(*topology_, {outputs::PanelOutputPolicy::primary, {}}, generation)
            .value();
    }

  private:
    outputs::OutputSurfaceRegistry registry_;
    std::optional<outputs::ReconciledTopologySnapshot> topology_;
};

struct PublicationLog final {
    std::vector<std::uint64_t> wallpapers;
    std::vector<std::uint64_t> failures;
};

[[nodiscard]] std::unique_ptr<DesktopWindowHost>
makeHost(const std::shared_ptr<TransactionGate> &gate, PublicationLog &log) {
    auto created = DesktopWindowHost::create(
        ":injected", [](std::string_view) { return QGuiApplication::primaryScreen(); },
        [&log](const auto &update, const auto &result) {
            (result ? log.wallpapers : log.failures)
                .push_back(update.snapshot()->wallpaper()->generation);
        },
        [gate](auto stopToken, auto operations) { return gate->run(stopToken, operations); });
    EXPECT_TRUE(created) << created.error().message;
    return created ? std::move(created).value() : nullptr;
}

TEST(DesktopWindowHostAsyncTest, SameTopologyWallpaperPublicationCompletesOnlyWhenQueued) {
    SameTopologyRegistry registry;
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    auto initial = controller.prepare(registry.settings(1U), scales, test::solidWallpaper(1U));
    ASSERT_TRUE(initial);
    ASSERT_TRUE(host->apply(initial.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.wallpapers.size() == 1U; }));
    controller.commit(initial.value());

    auto replacement = controller.prepare(registry.settings(2U), scales, test::solidWallpaper(2U));
    ASSERT_TRUE(replacement);
    ASSERT_TRUE(host->apply(replacement.value()));
    EXPECT_EQ(log.wallpapers, (std::vector<std::uint64_t>{1U}));
    ASSERT_TRUE(waitUntil([&] { return log.wallpapers.size() == 2U; }));
    EXPECT_EQ(log.wallpapers, (std::vector<std::uint64_t>{1U, 2U}));
    ASSERT_EQ(host->current()->wallpaper()->generation, 2U);
}

TEST(DesktopWindowHostAsyncTest, CoalescesBlockedSameTopologyWallpapersToNewest) {
    SameTopologyRegistry registry;
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    auto first = controller.prepare(registry.settings(1U), scales, test::solidWallpaper(1U));
    auto second = controller.prepare(registry.settings(2U), scales, test::solidWallpaper(2U));
    auto newest = controller.prepare(registry.settings(3U), scales, test::solidWallpaper(3U));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(newest);
    ASSERT_TRUE(host->apply(first.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    ASSERT_TRUE(host->apply(second.value()));
    ASSERT_TRUE(host->apply(newest.value()));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    EXPECT_TRUE(log.wallpapers.empty());
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.wallpapers.size() == 1U; }));

    EXPECT_EQ(log.wallpapers, (std::vector<std::uint64_t>{3U}));
    EXPECT_TRUE(log.failures.empty());
    ASSERT_EQ(host->current()->wallpaper()->generation, 3U);
    for (std::size_t index = 0U; index < 2U; ++index) {
        const auto operations = gate->transaction(index);
        ASSERT_EQ(operations.size(), 2U);
        EXPECT_TRUE(std::ranges::all_of(operations, [](const auto kind) {
            return kind == window::X11PropertyOperationKind::publishDesktop;
        }));
    }
    EXPECT_TRUE(gate->transaction(2U).empty());
}

TEST(DesktopWindowHostAsyncTest, RejectsOlderTopologyWhileNewerPublicationIsPending) {
    test::RegistryHarness registry;
    const auto olderRegistry = registry.observe(1U);
    const auto newerRegistry = registry.observe(2U, true, {0, 0, 480U, 768U});
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto older = controller.prepare(olderRegistry, scales, test::solidWallpaper(1U));
    auto newer = controller.prepare(newerRegistry, scales, test::solidWallpaper(2U));
    ASSERT_TRUE(older);
    ASSERT_TRUE(newer);
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(newer.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    EXPECT_FALSE(host->apply(older.value()));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.wallpapers.size() == 1U; }));
    EXPECT_EQ(log.wallpapers, (std::vector<std::uint64_t>{2U}));
}

TEST(DesktopWindowHostAsyncTest, FailedPublicationRetainsPriorWindowsAndDropsQueuedUpdate) {
    test::RegistryHarness registry;
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    auto initial = controller.prepare(registry.observe(1U), scales, test::solidWallpaper(1U));
    auto changed = controller.prepare(registry.observe(2U, true, {0, 0, 480U, 768U}), scales,
                                      test::solidWallpaper(2U));
    auto queued = controller.prepare(registry.observe(3U, true, {0, 0, 400U, 768U}), scales,
                                     test::solidWallpaper(3U));
    ASSERT_TRUE(initial);
    ASSERT_TRUE(changed);
    ASSERT_TRUE(queued);
    ASSERT_TRUE(host->apply(initial.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.wallpapers.size() == 1U; }));
    auto *left = host->window("LEFT-1");
    auto *right = host->window("RIGHT-1");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);

    ASSERT_TRUE(host->apply(changed.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->apply(queued.value()));
    gate->release(injectedFailure());
    ASSERT_TRUE(waitUntil([&] { return log.failures.size() == 1U; }));
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(log.failures, (std::vector<std::uint64_t>{2U}));
    EXPECT_FALSE(gate->started(3U));
    ASSERT_NE(host->current(), nullptr);
    EXPECT_EQ(host->current()->wallpaper()->generation, 1U);
    EXPECT_EQ(host->window("LEFT-1"), left);
    EXPECT_EQ(host->window("RIGHT-1"), right);
}

TEST(DesktopWindowHostAsyncTest, BlockingPublicationTeardownIsBounded) {
    SameTopologyRegistry registry;
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);
    auto update = controller.prepare(registry.settings(1U), scales, test::solidWallpaper(1U));
    ASSERT_TRUE(update);
    ASSERT_TRUE(host->apply(update.value()));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));

    const auto started = std::chrono::steady_clock::now();
    host.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
}

} // namespace
} // namespace prismdrake::shell::desktop

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
