#include "ShellOutputMonitor.hpp"

#include "OutputTopology.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] x11::OutputId outputId(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] x11::CrtcId crtcId(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] x11::OutputTopologySnapshot topology() {
    const auto root = x11::RootGeometry::create(1600U, 600U).value();
    std::vector<x11::OutputCandidate> outputs{
        {outputId(1U), crtcId(10U), 0, 0, 800U, 600U, "LEFT-1"},
        {outputId(2U), crtcId(20U), 800, 0, 800U, 600U, "RIGHT-1"},
    };
    return x11::buildOutputTopology(
               {true, root, outputs.size(), outputs.size(), 1U, std::move(outputs), outputId(1U)})
        .value();
}

struct TopologyStormState final {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t queries{0U};
    std::size_t validQuery{0U};
};

[[nodiscard]] x11::RandrTopologySnapshot wireTopology(bool valid) {
    return {x11::RandrTopologyStatus::unavailable,
            {0U, x11::WindowId::fromProtocol(1U).value(), valid ? 1600U : 0U, 600U},
            0U,
            0U,
            0U,
            {},
            std::nullopt};
}

class TopologyStormOperations final : public ShellOutputMonitorOperations {
  public:
    explicit TopologyStormOperations(std::shared_ptr<TopologyStormState> state)
        : state_(std::move(state)) {}

    foundation::Result<x11::RandrTopologySnapshot> query() override {
        std::size_t query = 0U;
        {
            std::scoped_lock lock{state_->mutex};
            query = ++state_->queries;
        }
        state_->changed.notify_all();
        if (state_->validQuery != 0U && query != state_->validQuery) {
            return foundation::Result<x11::RandrTopologySnapshot>::failure(
                {foundation::ErrorCode::validation_error, "injected stale topology failure",
                 "publish the later complete topology"});
        }
        return foundation::Result<x11::RandrTopologySnapshot>::success(wireTopology(true));
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return -1; }
    foundation::Result<bool> drainEvents() override {
        return foundation::Result<bool>::success(false);
    }
    void cancelWaits() noexcept override {}

  private:
    std::shared_ptr<TopologyStormState> state_;
};

struct BlockingQueryState final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool cancelled{false};
};

class BlockingQueryOperations final : public ShellOutputMonitorOperations {
  public:
    explicit BlockingQueryOperations(std::shared_ptr<BlockingQueryState> state)
        : state_(std::move(state)) {}

    foundation::Result<x11::RandrTopologySnapshot> query() override {
        std::unique_lock lock{state_->mutex};
        state_->entered = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [this] { return state_->cancelled; });
        return foundation::Result<x11::RandrTopologySnapshot>::failure(
            {foundation::ErrorCode::io_error, "injected cancelled query", "finish test"});
    }
    [[nodiscard]] int eventFileDescriptor() const noexcept override { return -1; }
    foundation::Result<bool> drainEvents() override {
        return foundation::Result<bool>::success(false);
    }
    void cancelWaits() noexcept override {
        {
            std::scoped_lock lock{state_->mutex};
            state_->cancelled = true;
        }
        state_->changed.notify_all();
    }

  private:
    std::shared_ptr<BlockingQueryState> state_;
};

TEST(ShellOutputMonitorTest, ObserverFailureKeepsTopologyAndPolicyRetryable) {
    bool reject = true;
    std::vector<outputs::SurfaceRegistrySnapshot> accepted;
    ShellOutputMonitor monitor{
        ":test",
        [&](const outputs::SurfaceRegistrySnapshot &snapshot) {
            if (reject) {
                return foundation::Result<void>::failure(
                    {foundation::ErrorCode::cancelled, "injected presentation failure", "retry"});
            }
            accepted.push_back(snapshot);
            return foundation::Result<void>::success();
        },
        {}};
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::primary, {}}, 10U));
    const std::array screens{
        outputs::QtScreenObservation{"LEFT-1", {0, 0, 800U, 600U}, 1.0},
        outputs::QtScreenObservation{"RIGHT-1", {800, 0, 800U, 600U}, 1.0},
    };

    EXPECT_FALSE(monitor.observeTopology(topology(), screens));
    reject = false;
    ASSERT_TRUE(monitor.observeTopology(topology(), screens));
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.back().surfaceGeneration(), 1U);
    ASSERT_EQ(accepted.back().panels().size(), 1U);

    reject = true;
    EXPECT_FALSE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 11U));
    reject = false;
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 11U));
    ASSERT_EQ(accepted.size(), 2U);
    EXPECT_EQ(accepted.back().topologyGeneration(), 1U);
    EXPECT_EQ(accepted.back().settingsGeneration(), 11U);
    EXPECT_EQ(accepted.back().surfaceGeneration(), 2U);
    EXPECT_EQ(accepted.back().panels().size(), 2U);
}

TEST(ShellOutputMonitorTest, NewSettingsOwnerRestartsGenerationWithoutLosingTopology) {
    std::vector<outputs::SurfaceRegistrySnapshot> accepted;
    ShellOutputMonitor monitor{":test",
                               [&](const outputs::SurfaceRegistrySnapshot &snapshot) {
                                   accepted.push_back(snapshot);
                                   return foundation::Result<void>::success();
                               },
                               {}};
    const std::array screens{
        outputs::QtScreenObservation{"LEFT-1", {0, 0, 800U, 600U}, 1.0},
        outputs::QtScreenObservation{"RIGHT-1", {800, 0, 800U, 600U}, 1.0},
    };

    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 3U));
    ASSERT_TRUE(monitor.observeTopology(topology(), screens));
    ASSERT_EQ(accepted.size(), 1U);
    const auto previousSurfaceGeneration = accepted.back().surfaceGeneration();

    EXPECT_FALSE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 1U));
    monitor.beginSettingsOwnerEpoch();
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 1U));
    ASSERT_EQ(accepted.size(), 2U);
    EXPECT_EQ(accepted.back().topologyGeneration(), 1U);
    EXPECT_EQ(accepted.back().settingsGeneration(), 1U);
    EXPECT_GT(accepted.back().surfaceGeneration(), previousSurfaceGeneration);
    EXPECT_EQ(accepted.back().panels().size(), 2U);

    EXPECT_FALSE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 1U));
}

TEST(ShellOutputMonitorTest, TopologyStormQueuesOnlyTheNewestStateForTheOwner) {
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    constexpr std::size_t refreshCount = 128U;
    auto state = std::make_shared<TopologyStormState>();
    state->validQuery = refreshCount + 1U;
    std::vector<outputs::SurfaceRegistrySnapshot> accepted;
    std::vector<foundation::Error> failures;
    ShellOutputMonitor monitor{
        [state] {
            return foundation::Result<std::unique_ptr<ShellOutputMonitorOperations>>::success(
                std::make_unique<TopologyStormOperations>(state));
        },
        [&accepted](const outputs::SurfaceRegistrySnapshot &snapshot) {
            accepted.push_back(snapshot);
            return foundation::Result<void>::success();
        },
        [&failures](const foundation::Error &error, bool) { failures.push_back(error); }};
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::primary, {}}, 1U));
    ASSERT_TRUE(monitor.start());

    for (std::size_t expected = 1U; expected <= refreshCount + 1U; ++expected) {
        if (expected > 1U) {
            monitor.requestRefresh();
        }
        std::unique_lock lock{state->mutex};
        ASSERT_TRUE(state->changed.wait_for(lock, std::chrono::seconds{1},
                                            [&] { return state->queries >= expected; }));
    }
    EXPECT_TRUE(accepted.empty());
    EXPECT_TRUE(failures.empty());

    QElapsedTimer elapsed;
    elapsed.start();
    bool heartbeat = false;
    QEventLoop loop;
    QTimer::singleShot(0, &loop, [&] {
        heartbeat = true;
        if (!accepted.empty() || !failures.empty()) {
            loop.quit();
        }
    });
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!accepted.empty() || !failures.empty()) {
            loop.quit();
        }
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();

    monitor.stop();
    EXPECT_TRUE(heartbeat);
    EXPECT_LT(elapsed.elapsed(), 100);
    EXPECT_TRUE(failures.empty());
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.front().topologyGeneration(), 1U);
}

TEST(ShellOutputMonitorTest, TeardownInterruptsAStalledReplyBearingQuery) {
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    auto state = std::make_shared<BlockingQueryState>();
    ShellOutputMonitor monitor{
        [state] {
            return foundation::Result<std::unique_ptr<ShellOutputMonitorOperations>>::success(
                std::make_unique<BlockingQueryOperations>(state));
        },
        [](const outputs::SurfaceRegistrySnapshot &) {
            return foundation::Result<void>::success();
        },
        {}};
    ASSERT_TRUE(monitor.start());
    {
        std::unique_lock lock{state->mutex};
        ASSERT_TRUE(
            state->changed.wait_for(lock, std::chrono::seconds{1}, [&] { return state->entered; }));
    }

    const auto started = std::chrono::steady_clock::now();
    monitor.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{100});
}

TEST(ShellOutputMonitorTest, StopAndStartReuseTheOperationsFactory) {
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    auto state = std::make_shared<TopologyStormState>();
    std::atomic<int> factoryCalls{0};
    ShellOutputMonitor monitor{
        [state, &factoryCalls] {
            factoryCalls.fetch_add(1, std::memory_order_relaxed);
            return foundation::Result<std::unique_ptr<ShellOutputMonitorOperations>>::success(
                std::make_unique<TopologyStormOperations>(state));
        },
        [](const outputs::SurfaceRegistrySnapshot &) {
            return foundation::Result<void>::success();
        },
        {}};

    for (std::size_t expectedQueries = 1U; expectedQueries <= 2U; ++expectedQueries) {
        ASSERT_TRUE(monitor.start());
        std::unique_lock lock{state->mutex};
        ASSERT_TRUE(state->changed.wait_for(lock, std::chrono::seconds{1},
                                            [&] { return state->queries >= expectedQueries; }));
        lock.unlock();
        monitor.stop();
    }
    EXPECT_EQ(factoryCalls.load(std::memory_order_relaxed), 2);
}

} // namespace
} // namespace prismdrake::shell::runtime
