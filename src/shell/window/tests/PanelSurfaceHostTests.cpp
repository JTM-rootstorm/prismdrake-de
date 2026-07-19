#include "PanelSurfaceHost.hpp"

#include "OutputSurfaceRegistry.hpp"
#include "OutputTopology.hpp"
#include "ScreenReconciliation.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickView>
#include <QScreen>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::window {
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
    return Result<void>::failure({ErrorCode::io_error, "Injected X11 publication failure.",
                                  "Retain the prior complete surface generation."});
}

[[nodiscard]] Result<void> cancelledTransaction() {
    return Result<void>::failure({ErrorCode::cancelled, "Injected X11 publication cancelled.",
                                  "Discard the incomplete surface generation."});
}

class TransactionGate final {
  public:
    [[nodiscard]] Result<void> run(std::stop_token stopToken,
                                   std::span<const X11PropertyOperation> operations) {
        {
            std::scoped_lock lock{mutex_};
            transactions_.emplace_back();
            for (const auto &operation : operations) {
                transactions_.back().push_back(operation.kind);
            }
        }
        if (std::ranges::all_of(operations, [](const auto &operation) {
                return operation.kind == X11PropertyOperationKind::removeDock ||
                       operation.kind == X11PropertyOperationKind::removeDesktop;
            })) {
            return Result<void>::success();
        }
        std::unique_lock lock{mutex_};
        const auto sequence = ++started_;
        changed_.notify_all();
        changed_.wait(lock, stopToken, [&] { return stopping_ || outcomes_.size() >= sequence; });
        if (stopToken.stop_requested() || stopping_) {
            return cancelledTransaction();
        }
        auto result = std::move(outcomes_[sequence - 1U]);
        ++finished_;
        changed_.notify_all();
        return result;
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

    [[nodiscard]] bool finished(std::size_t count) const {
        std::scoped_lock lock{mutex_};
        return finished_ >= count;
    }

    [[nodiscard]] std::vector<X11PropertyOperationKind> transaction(std::size_t index) const {
        std::scoped_lock lock{mutex_};
        return index < transactions_.size() ? transactions_[index]
                                            : std::vector<X11PropertyOperationKind>{};
    }

    ~TransactionGate() {
        {
            std::scoped_lock lock{mutex_};
            stopping_ = true;
        }
        changed_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Result<void>> outcomes_;
    std::vector<std::vector<X11PropertyOperationKind>> transactions_;
    std::size_t started_{0U};
    std::size_t finished_{0U};
    bool stopping_{false};
};

class RegistryFixture final {
  public:
    RegistryFixture() : registry_(outputs::OutputSurfaceRegistry::create(48U).value()) {}

    [[nodiscard]] outputs::SurfaceRegistrySnapshot empty(std::uint64_t generation) {
        const auto root = x11::RootGeometry::create(1280U, 1024U).value();
        const auto topology =
            x11::buildOutputTopology({true, root, 0U, 0U, 0U, {}, std::nullopt}).value();
        const auto reconciled = outputs::reconcileScreens(generation, topology, {}).value();
        return registry_.observe(reconciled, {outputs::PanelOutputPolicy::primary, {}}, 1U).value();
    }

    [[nodiscard]] outputs::SurfaceRegistrySnapshot
    one(std::uint64_t generation, std::uint32_t width = 1024U, std::uint32_t height = 768U) {
        const auto root = x11::RootGeometry::create(1280U, 1024U).value();
        const auto output = x11::OutputId::fromProtocol(1U).value();
        const auto crtc = x11::CrtcId::fromProtocol(2U).value();
        const std::vector candidates{
            x11::OutputCandidate{output, crtc, 0, 0, width, height, "TEST-1"}};
        const auto topology =
            x11::buildOutputTopology({true, root, 1U, 1U, 1U, candidates, output}).value();
        const std::vector screens{
            outputs::QtScreenObservation{"TEST-1", {0, 0, width, height}, 1.0}};
        const auto reconciled = outputs::reconcileScreens(generation, topology, screens).value();
        return registry_.observe(reconciled, {outputs::PanelOutputPolicy::primary, {}}, 1U).value();
    }

    [[nodiscard]] outputs::SurfaceRegistrySnapshot many(std::uint64_t generation,
                                                        std::uint32_t outputWidth) {
        constexpr std::uint32_t outputCount = 48U;
        constexpr std::uint32_t outputHeight = 768U;
        const auto root =
            x11::RootGeometry::create(outputCount * outputWidth, outputHeight).value();
        std::vector<x11::OutputCandidate> candidates;
        std::vector<outputs::QtScreenObservation> screens;
        candidates.reserve(outputCount);
        screens.reserve(outputCount);
        for (std::uint32_t index = 0U; index < outputCount; ++index) {
            const auto name = "TEST-" + std::to_string(index + 1U);
            candidates.push_back({x11::OutputId::fromProtocol(index + 1U).value(),
                                  x11::CrtcId::fromProtocol(index + 101U).value(),
                                  static_cast<std::int32_t>(index * outputWidth), 0, outputWidth,
                                  outputHeight, name});
            screens.push_back(
                {name,
                 {static_cast<std::int32_t>(index * outputWidth), 0, outputWidth, outputHeight},
                 1.0});
        }
        const auto topology =
            x11::buildOutputTopology({true, root, outputCount, outputCount, outputCount, candidates,
                                      candidates.front().outputId})
                .value();
        const auto reconciled = outputs::reconcileScreens(generation, topology, screens).value();
        return registry_.observe(reconciled, {outputs::PanelOutputPolicy::all, {}}, 1U).value();
    }

  private:
    outputs::OutputSurfaceRegistry registry_;
};

struct PublicationLog final {
    std::vector<std::uint64_t> successful;
    std::vector<std::uint64_t> failed;

    void append(const outputs::SurfaceRegistrySnapshot &snapshot, const Result<void> &result) {
        (result ? successful : failed).push_back(snapshot.surfaceGeneration());
    }
};

[[nodiscard]] std::unique_ptr<PanelSurfaceHost>
makeHost(const std::shared_ptr<TransactionGate> &gate, PublicationLog &log) {
    auto created = PanelSurfaceHost::create(
        ":injected", [](std::string_view) { return QGuiApplication::primaryScreen(); },
        [](std::string_view, QScreen &screen) -> Result<std::unique_ptr<QQuickView>> {
            auto view = std::make_unique<QQuickView>();
            view->setScreen(&screen);
            return Result<std::unique_ptr<QQuickView>>::success(std::move(view));
        },
        [&log](const auto &snapshot, const auto &result) { log.append(snapshot, result); },
        [gate](auto stopToken, auto operations) { return gate->run(stopToken, operations); });
    EXPECT_TRUE(created) << created.error().message;
    return created ? std::move(created).value() : nullptr;
}

TEST(PanelSurfaceHostTest, QueuesZeroOutputNoOpPublicationUntilAfterApplyReturns) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.empty(1U)));
    EXPECT_TRUE(log.successful.empty());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_EQ(host->currentGeneration(), 1U);
}

TEST(PanelSurfaceHostTest, BlockingTransactionLeavesOwnerResponsiveAndTeardownBounded) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    const auto applyStarted = std::chrono::steady_clock::now();
    ASSERT_TRUE(host->apply(registry.one(1U)));
    EXPECT_LT(std::chrono::steady_clock::now() - applyStarted, 100ms);
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    bool heartbeat = false;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [&] { heartbeat = true; }, Qt::QueuedConnection);
    ASSERT_TRUE(waitUntil([&] { return heartbeat; }));
    EXPECT_TRUE(log.successful.empty());

    const auto teardownStarted = std::chrono::steady_clock::now();
    host.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - teardownStarted, 500ms);
}

TEST(PanelSurfaceHostTest, CoalescesQueuedSnapshotsAndPublishesOnlyLatestGeneration) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    ASSERT_TRUE(host->apply(registry.one(2U, 960U, 720U)));
    ASSERT_TRUE(host->apply(registry.one(3U, 800U, 600U)));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    EXPECT_TRUE(log.successful.empty());
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{3U}));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_EQ(host->currentGeneration(), 3U);
}

TEST(PanelSurfaceHostTest, MaximumOutputReplacementDoesNotQueueStaleXidCleanup) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.many(1U, 100U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    ASSERT_TRUE(host->apply(registry.many(2U, 96U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->apply(registry.many(3U, 92U)));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->started(3U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 2U; }));

    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{1U, 3U}));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_EQ(host->currentGeneration(), 3U);
    for (std::size_t index = 0U; index < 3U; ++index) {
        const auto operations = gate->transaction(index);
        ASSERT_EQ(operations.size(), 48U);
        EXPECT_TRUE(std::ranges::all_of(operations, [](const auto kind) {
            return kind == X11PropertyOperationKind::publishDock;
        }));
    }
    EXPECT_TRUE(gate->transaction(3U).empty());
}

TEST(PanelSurfaceHostTest, FailedPublicationRetainsPriorGenerationAndDropsQueuedWork) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    auto *prior = host->view("TEST-1");
    ASSERT_NE(prior, nullptr);

    ASSERT_TRUE(host->apply(registry.one(2U, 960U, 720U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->apply(registry.one(3U, 800U, 600U)));
    gate->release(injectedFailure());
    ASSERT_TRUE(waitUntil([&] { return log.failed.size() == 1U; }));

    EXPECT_EQ(log.failed, (std::vector<std::uint64_t>{2U}));
    EXPECT_EQ(host->currentGeneration(), 1U);
    EXPECT_EQ(host->view("TEST-1"), prior);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_FALSE(gate->started(3U));
}

TEST(PanelSurfaceHostTest, StaleFocusCompletionAfterTopologyReplacementIsBenign) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    ASSERT_TRUE(host->apply(registry.one(2U, 960U, 720U)));
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1"));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->started(3U); }));
    ASSERT_TRUE(waitUntil([&] { return host->currentGeneration() == 2U; }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->finished(3U); }));
    QCoreApplication::processEvents();

    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{1U, 2U}));
    EXPECT_TRUE(log.failed.empty());
}

TEST(PanelSurfaceHostTest, NoReplacementTopologyQueuedBehindFocusRetainsOldPublication) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    auto *original = host->view("TEST-1");
    ASSERT_NE(original, nullptr);

    std::size_t readyCallbacks = 0U;
    QQuickView *readyView = nullptr;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &view) {
        ++readyCallbacks;
        readyView = &view;
    }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->apply(registry.one(2U)));
    EXPECT_EQ(gate->transaction(1U),
              (std::vector<X11PropertyOperationKind>{X11PropertyOperationKind::publishDock}));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 2U; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(3U); }));
    EXPECT_EQ(readyCallbacks, 0U);
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return readyCallbacks == 1U; }));

    EXPECT_EQ(host->currentGeneration(), 2U);
    EXPECT_NE(host->view("TEST-1"), original);
    EXPECT_EQ(host->view("TEST-1"), readyView);
    EXPECT_TRUE(log.failed.empty());
}

TEST(PanelSurfaceHostTest, ClearCancelsInFlightFocusCompletion) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    bool ready = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { ready = true; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));

    host->clear();
    gate->release(injectedFailure());
    ASSERT_TRUE(waitUntil([&] { return gate->finished(2U); }));
    QCoreApplication::processEvents();
    EXPECT_EQ(host->currentGeneration(), 0U);
    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{1U}));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_FALSE(ready);
}

TEST(PanelSurfaceHostTest, ClearThenApplyIgnoresStaleFocusFailureAndPublishesNewSnapshot) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    bool staleReady = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { staleReady = true; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));

    host->clear();
    ASSERT_TRUE(host->apply(registry.one(2U, 960U, 720U)));
    gate->release(injectedFailure());
    ASSERT_TRUE(waitUntil([&] { return gate->started(3U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 2U; }));

    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{1U, 2U}));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_FALSE(staleReady);
    EXPECT_EQ(host->currentGeneration(), 2U);
    ASSERT_NE(host->view("TEST-1"), nullptr);
    for (int iteration = 0; iteration < 20; ++iteration) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_FALSE(gate->started(4U));
}

TEST(PanelSurfaceHostTest, ClearThenApplyIgnoresStaleApplyFailureAndPublishesNewSnapshot) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    host->clear();
    ASSERT_TRUE(host->apply(registry.one(2U, 960U, 720U)));

    gate->release(injectedFailure());
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    EXPECT_EQ(log.successful, (std::vector<std::uint64_t>{2U}));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_EQ(host->currentGeneration(), 2U);
    ASSERT_NE(host->view("TEST-1"), nullptr);
}

TEST(PanelSurfaceHostTest, RapidFocusReleaseCoalescesToUnfocusedMappedPanel) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);

    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    bool ready = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { ready = true; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->releaseKeyboardAccess());

    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->finished(2U); }));
    QCoreApplication::processEvents();
    auto *panel = host->view("TEST-1");
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(panel->flags() & Qt::WindowDoesNotAcceptFocus);
    EXPECT_FALSE(gate->started(3U));
    EXPECT_TRUE(log.failed.empty());
    EXPECT_FALSE(ready);
}

TEST(PanelSurfaceHostTest, FocusReadyRunsOnlyAfterFocusableReplacementIsCurrent) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    auto *oldView = host->view("TEST-1");
    ASSERT_NE(oldView, nullptr);

    bool callbackInvoked = false;
    bool currentMatched = false;
    bool focusable = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &view) {
        callbackInvoked = true;
        currentMatched = host->view("TEST-1") == &view;
        focusable = !(view.flags() & Qt::WindowDoesNotAcceptFocus);
    }));
    EXPECT_FALSE(callbackInvoked);
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    EXPECT_FALSE(callbackInvoked);
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return callbackInvoked; }));

    EXPECT_TRUE(currentMatched);
    EXPECT_TRUE(focusable);
    EXPECT_NE(host->view("TEST-1"), oldView);
}

TEST(PanelSurfaceHostTest, RemovedOutputCancelsQueuedFocusWithoutPublicationFailure) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    bool ready = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { ready = true; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->apply(registry.empty(2U)));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 2U; }));

    EXPECT_EQ(host->currentGeneration(), 2U);
    EXPECT_EQ(host->view("TEST-1"), nullptr);
    EXPECT_FALSE(ready);
    EXPECT_TRUE(log.failed.empty());
}

TEST(PanelSurfaceHostTest, AlreadyFocusableRequestKeepsViewAndRunsReadyWithoutWorker) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1"));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] {
        auto *view = host->view("TEST-1");
        return view && !(view->flags() & Qt::WindowDoesNotAcceptFocus);
    }));
    auto *focusedView = host->view("TEST-1");

    QQuickView *readyView = nullptr;
    ASSERT_TRUE(
        host->requestKeyboardAccess("TEST-1", [&](QQuickView &view) { readyView = &view; }));
    EXPECT_EQ(readyView, focusedView);
    EXPECT_EQ(host->view("TEST-1"), focusedView);
    EXPECT_FALSE(gate->started(3U));
}

TEST(PanelSurfaceHostTest, NewerFocusRequestSuppressesStaleReadyCallback) {
    RegistryFixture registry;
    auto gate = std::make_shared<TransactionGate>();
    PublicationLog log;
    auto host = makeHost(gate, log);
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(host->apply(registry.one(1U)));
    ASSERT_TRUE(waitUntil([&] { return gate->started(1U); }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return log.successful.size() == 1U; }));

    bool staleReady = false;
    bool latestReady = false;
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { staleReady = true; }));
    ASSERT_TRUE(waitUntil([&] { return gate->started(2U); }));
    ASSERT_TRUE(host->requestKeyboardAccess("TEST-1", [&](QQuickView &) { latestReady = true; }));
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return gate->started(3U); }));
    EXPECT_FALSE(staleReady);
    EXPECT_FALSE(latestReady);
    gate->release(Result<void>::success());
    ASSERT_TRUE(waitUntil([&] { return latestReady; }));
    EXPECT_FALSE(staleReady);
}

} // namespace
} // namespace prismdrake::shell::window

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
