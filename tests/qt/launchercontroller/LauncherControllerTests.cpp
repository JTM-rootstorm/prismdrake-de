#include "LauncherController.hpp"

#include "ApplicationRootMonitor.hpp"
#include "DesktopFileId.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::launcher::controller {
namespace {

using foundation::ErrorCode;
using prismdrake::launcher::ApplicationCatalogDecision;
using prismdrake::launcher::ApplicationCatalogEligibilityReason;
using prismdrake::launcher::ApplicationCatalogSnapshot;
using prismdrake::launcher::ApplicationSearchSnapshot;
using prismdrake::launcher::DesktopEntry;
using prismdrake::launcher::DesktopEntryDiscoverySnapshot;
using prismdrake::launcher::DesktopEntryVisibilityReason;
using prismdrake::launcher::DiscoveredDesktopEntry;

[[nodiscard]] DiscoveredDesktopEntry entry(std::string id, std::string name,
                                           std::string exec = "/bin/echo literal") {
    auto identifier = prismdrake::launcher::deriveDesktopFileId(id);
    EXPECT_TRUE(identifier);
    DesktopEntry value;
    value.name = std::move(name);
    value.exec = std::move(exec);
    auto location =
        prismdrake::launcher::makeDiscoveredDesktopFileLocation("/fixture/applications", id, 0U);
    EXPECT_TRUE(location);
    return {std::move(identifier).value(), std::move(value),
            DesktopEntryVisibilityReason::visibleByDefault, std::move(location).value()};
}

[[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot> catalog(std::uint64_t generation) {
    std::vector<DiscoveredDesktopEntry> entries{entry("alpha.desktop", "Alpha"),
                                                entry("beta.desktop", "Beta")};
    entries[0].entry.actions.push_back(
        {"NewWindow", "New Window", std::nullopt, "/bin/echo new-window"});
    std::vector<std::size_t> indices{0U, 1U};
    auto discovery = std::make_shared<const DesktopEntryDiscoverySnapshot>(
        DesktopEntryDiscoverySnapshot{std::move(entries), indices, {}, 2U, true, false, false});
    return std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
        generation,
        discovery,
        {{0U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec},
         {1U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec}},
        indices,
        2U,
        2U,
        true});
}

class FakeIndexBackend final : public LauncherIndexBackend {
  public:
    foundation::Result<std::shared_ptr<const ApplicationCatalogSnapshot>>
    refresh(const LauncherControllerOptions &, std::uint64_t generation,
            const foundation::CancellationToken &) override {
        refresh_thread = std::this_thread::get_id();
        ++refresh_calls;
        if (fail_refresh.exchange(false, std::memory_order_acq_rel)) {
            return foundation::Result<std::shared_ptr<const ApplicationCatalogSnapshot>>::failure(
                {ErrorCode::io_error, "fixture refresh failed", "retry the fixture refresh"});
        }
        return foundation::Result<std::shared_ptr<const ApplicationCatalogSnapshot>>::success(
            catalog(generation));
    }

    foundation::Result<std::shared_ptr<const ApplicationSearchSnapshot>>
    search(std::shared_ptr<const ApplicationCatalogSnapshot> source,
           std::uint64_t requestGeneration,
           const prismdrake::launcher::ApplicationSearchQuery &query,
           const foundation::CancellationToken &cancellation) override {
        ++search_calls;
        search_started.store(true, std::memory_order_release);
        if (hold_search.exchange(false, std::memory_order_acq_rel)) {
            while (!release_search.load(std::memory_order_acquire)) {
                if (!ignore_cancellation && cancellation.isCancellationRequested()) {
                    ++cancelled_searches;
                    return foundation::Result<std::shared_ptr<const ApplicationSearchSnapshot>>::
                        failure({ErrorCode::cancelled, "fixture search cancelled",
                                 "run the replacement search"});
                }
                std::this_thread::yield();
            }
        }
        auto operation = prismdrake::launcher::createApplicationSearch(
            std::move(source), requestGeneration, query,
            prismdrake::launcher::maximumApplicationSearchResults);
        if (!operation) {
            return foundation::Result<std::shared_ptr<const ApplicationSearchSnapshot>>::failure(
                operation.error());
        }
        foundation::CancellationSource uncancelled;
        return operation.value().advance(prismdrake::launcher::maximumApplicationSearchWorkUnits,
                                         uncancelled.token());
    }

    std::atomic_int refresh_calls{0};
    std::atomic_int search_calls{0};
    std::atomic_int cancelled_searches{0};
    std::atomic_bool hold_search{false};
    std::atomic_bool release_search{false};
    std::atomic_bool search_started{false};
    std::atomic_bool fail_refresh{false};
    bool ignore_cancellation{false};
    std::thread::id refresh_thread;
};

class FakeLaunchBackend final : public LauncherLaunchBackend {
  public:
    foundation::Result<void> launch(const DiscoveredDesktopEntry &entry,
                                    std::optional<std::string> action,
                                    const prismdrake::launcher::ProcessLaunchContext &,
                                    const foundation::CancellationToken &) override {
        ++calls;
        captured_exec = entry.entry.exec;
        captured_action = std::move(action);
        return foundation::Result<void>::success();
    }

    std::atomic_int calls{0};
    std::optional<std::string> captured_exec;
    std::optional<std::string> captured_action;
};

class FakeApplicationActivator final : public LauncherApplicationActivator {
  public:
    foundation::Result<void> activate(const prismdrake::launcher::DesktopFileId &desktopFileId,
                                      std::optional<std::string_view> actionId,
                                      const foundation::CancellationToken &) override {
        ++calls;
        captured_id = desktopFileId.value();
        captured_action = actionId ? std::optional<std::string>{*actionId} : std::nullopt;
        if (fail) {
            return foundation::Result<void>::failure(
                {ErrorCode::not_found, "fixture activation failed", "use a permitted fallback"});
        }
        return foundation::Result<void>::success();
    }

    int calls{0};
    bool fail{false};
    std::string captured_id;
    std::optional<std::string> captured_action;
};

struct BlockingCollectorState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ApplicationWatchCandidates> results;
    std::size_t calls{0U};
    std::size_t releasedCalls{0U};
    bool stopObserved{false};
};

class BlockingWatchCandidateCollector final : public ApplicationWatchCandidateCollector {
  public:
    explicit BlockingWatchCandidateCollector(std::shared_ptr<BlockingCollectorState> state)
        : state_(std::move(state)) {}

    foundation::Result<ApplicationWatchCandidates>
    collect(const prismdrake::launcher::ApplicationPaths &, std::size_t, std::size_t,
            std::stop_token stopToken) override {
        std::unique_lock lock(state_->mutex);
        const auto call = ++state_->calls;
        state_->condition.notify_all();
        std::stop_callback wake{stopToken, [state = state_] { state->condition.notify_all(); }};
        state_->condition.wait(
            lock, [&] { return stopToken.stop_requested() || state_->releasedCalls >= call; });
        if (stopToken.stop_requested()) {
            state_->stopObserved = true;
            state_->condition.notify_all();
            return foundation::Result<ApplicationWatchCandidates>::failure(
                {ErrorCode::cancelled, "fixture collection stopped", "destroy the fixture"});
        }
        return foundation::Result<ApplicationWatchCandidates>::success(
            state_->results.at(std::min(call - 1U, state_->results.size() - 1U)));
    }

  private:
    std::shared_ptr<BlockingCollectorState> state_;
};

class DegradedWatchCandidateCollector final : public ApplicationWatchCandidateCollector {
  public:
    explicit DegradedWatchCandidateCollector(std::filesystem::path path) : path_(std::move(path)) {}

    foundation::Result<ApplicationWatchCandidates>
    collect(const prismdrake::launcher::ApplicationPaths &, std::size_t, std::size_t,
            std::stop_token) override {
        ++calls;
        return foundation::Result<ApplicationWatchCandidates>::success({{path_}, true});
    }

    std::atomic_size_t calls{0U};

  private:
    std::filesystem::path path_;
};

struct BlockingWatcherState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t calls{0U};
    bool stopObserved{false};
    std::thread::id workerThread;
};

class BlockingApplicationPathWatcher final : public ApplicationPathWatcher {
  public:
    explicit BlockingApplicationPathWatcher(std::shared_ptr<BlockingWatcherState> state)
        : state_(std::move(state)) {}

    void setChangeCallback(ChangeCallback callback) override { callback_ = std::move(callback); }

    foundation::Result<ApplicationWatchRegistration>
    replace(const std::vector<std::filesystem::path> &, std::stop_token stopToken) override {
        std::unique_lock lock(state_->mutex);
        ++state_->calls;
        state_->workerThread = std::this_thread::get_id();
        state_->condition.notify_all();
        std::stop_callback wake{stopToken, [state = state_] { state->condition.notify_all(); }};
        state_->condition.wait(lock, [&] { return stopToken.stop_requested(); });
        state_->stopObserved = true;
        state_->condition.notify_all();
        return foundation::Result<ApplicationWatchRegistration>::failure(
            {ErrorCode::cancelled, "fixture watch registration stopped", "destroy the fixture"});
    }

  private:
    std::shared_ptr<BlockingWatcherState> state_;
    ChangeCallback callback_;
};

[[nodiscard]] std::size_t collectorCalls(const std::shared_ptr<BlockingCollectorState> &state) {
    std::lock_guard lock(state->mutex);
    return state->calls;
}

void releaseCollectorCall(const std::shared_ptr<BlockingCollectorState> &state, std::size_t call) {
    {
        std::lock_guard lock(state->mutex);
        state->releasedCalls = std::max(state->releasedCalls, call);
    }
    state->condition.notify_all();
}

class CapturingExecutor final : public LauncherProcessExecutor {
  public:
    foundation::Result<void> execute(const prismdrake::launcher::ProcessLaunchPlan &plan,
                                     const foundation::CancellationToken &) override {
        plans.push_back(plan);
        return foundation::Result<void>::success();
    }

    std::vector<prismdrake::launcher::ProcessLaunchPlan> plans;
};

[[nodiscard]] LauncherControllerOptions
options(std::filesystem::path applicationRoot = "/tmp/prismdrake-controller-missing-applications",
        bool automaticMonitoring = false) {
    auto desktop = prismdrake::launcher::parseCurrentDesktopContext("Prismdrake");
    EXPECT_TRUE(desktop);
    prismdrake::launcher::ProcessLaunchContext launch{
        {"/bin:/usr/bin", "/"}, "/tmp", {"LANG=C"}, std::nullopt};
    return {{{std::move(applicationRoot)}},
            {"C"},
            std::move(desktop).value(),
            {},
            {"/bin:/usr/bin", "/"},
            std::move(launch),
            automaticMonitoring};
}

struct ControllerFixture final {
    std::unique_ptr<LauncherController> controller;
    FakeIndexBackend *index;
    FakeLaunchBackend *launch;
};

[[nodiscard]] ControllerFixture makeController() {
    auto index = std::make_unique<FakeIndexBackend>();
    auto launch = std::make_unique<FakeLaunchBackend>();
    auto *indexPointer = index.get();
    auto *launchPointer = launch.get();
    auto created = LauncherController::create(options(), std::move(index), std::move(launch));
    EXPECT_TRUE(created);
    return {std::move(created).value(), indexPointer, launchPointer};
}

void publishInitial(ControllerFixture &fixture) {
    ASSERT_TRUE(fixture.controller->refresh());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 2, 2000);
}

TEST(ApplicationRootMonitorTest, CoalescesDesktopEntryChangesAndRecoversRootReplacement) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("applications"));
    ASSERT_TRUE(QDir{}.mkpath(root));
    auto monitor = ApplicationRootMonitor::create(
        {{{root.toStdString()}}},
        {32U, std::chrono::milliseconds{25}, std::chrono::milliseconds{25}});
    ASSERT_TRUE(monitor);
    QSignalSpy refresh(monitor.value().get(), &ApplicationRootMonitor::refreshRequested);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 1, 1000);

    QFile first{root + QStringLiteral("/one.desktop")};
    ASSERT_TRUE(first.open(QIODevice::WriteOnly));
    first.write("[Desktop Entry]\n");
    first.close();
    QFile second{root + QStringLiteral("/two.desktop")};
    ASSERT_TRUE(second.open(QIODevice::WriteOnly));
    second.write("[Desktop Entry]\n");
    second.close();
    QTRY_COMPARE_WITH_TIMEOUT(refresh.count(), 1, 1000);

    refresh.clear();
    ASSERT_TRUE(QDir{root}.removeRecursively());
    QTRY_COMPARE_WITH_TIMEOUT(refresh.count(), 1, 1000);
    refresh.clear();
    ASSERT_TRUE(QDir{}.mkpath(root));
    QTRY_COMPARE_WITH_TIMEOUT(refresh.count(), 1, 1000);
    EXPECT_FALSE(monitor.value()->degraded());
}

TEST(ApplicationRootMonitorTest, BoundsWatchCapacityAndExplicitlyRecoversExhaustion) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("applications"));
    ASSERT_TRUE(QDir{}.mkpath(root + QStringLiteral("/nested")));
    QFile desktop{root + QStringLiteral("/nested/tool.desktop")};
    ASSERT_TRUE(desktop.open(QIODevice::WriteOnly));
    desktop.write("[Desktop Entry]\n");
    desktop.close();

    auto monitor =
        ApplicationRootMonitor::create({{{root.toStdString()}}}, {1U, std::chrono::milliseconds{20},
                                                                  std::chrono::milliseconds{20}});
    ASSERT_TRUE(monitor);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.value()->degraded(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 1, 1000);

    ASSERT_TRUE(QDir{root + QStringLiteral("/nested")}.removeRecursively());
    monitor.value()->requestRecovery();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.value()->degraded(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 1, 1000);
}

TEST(ApplicationRootMonitorTest, PrioritizesEveryRootBeforeRecursiveDescendants) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString firstRoot = temporary.filePath(QStringLiteral("first/applications"));
    const QString secondRoot = temporary.filePath(QStringLiteral("second/applications"));
    ASSERT_TRUE(QDir{}.mkpath(firstRoot + QStringLiteral("/nested")));
    ASSERT_TRUE(QDir{}.mkpath(secondRoot + QStringLiteral("/nested")));

    auto monitor = ApplicationRootMonitor::create(
        {{{firstRoot.toStdString(), secondRoot.toStdString()}}},
        {2U, std::chrono::milliseconds{20}, std::chrono::milliseconds{20}});
    ASSERT_TRUE(monitor);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.value()->degraded(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 2, 1000);
    QSignalSpy refresh(monitor.value().get(), &ApplicationRootMonitor::refreshRequested);

    ASSERT_TRUE(QDir{secondRoot}.removeRecursively());
    QTRY_COMPARE_WITH_TIMEOUT(refresh.count(), 1, 1000);
}

TEST(ApplicationRootMonitorTest, DiscoveryWorkBudgetCountsIrrelevantVisitedNodes) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("applications"));
    ASSERT_TRUE(QDir{}.mkpath(root));
    for (int index = 0; index < 32; ++index) {
        QFile irrelevant{root + QStringLiteral("/irrelevant-%1.txt").arg(index)};
        ASSERT_TRUE(irrelevant.open(QIODevice::WriteOnly));
        irrelevant.write("ignored\n");
    }

    auto monitor = ApplicationRootMonitor::create(
        {{{root.toStdString()}}},
        {64U, std::chrono::milliseconds{20}, std::chrono::milliseconds{20}, 4U});
    ASSERT_TRUE(monitor);
    QTRY_VERIFY_WITH_TIMEOUT(monitor.value()->degraded(), 1000);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 1, 1000);
}

TEST(ApplicationRootMonitorTest, BlockingWatchBoundaryIsWorkerOwnedAndCooperativelyStopped) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("applications")).toStdString();
    ASSERT_TRUE(QDir{}.mkpath(QString::fromStdString(root)));
    const auto state = std::make_shared<BlockingWatcherState>();
    const auto ownerThread = std::this_thread::get_id();

    const auto createStarted = std::chrono::steady_clock::now();
    auto monitor = ApplicationRootMonitor::create(
        {{{root}}}, {8U, std::chrono::milliseconds{10}, std::chrono::milliseconds{10}}, nullptr, {},
        [state] { return std::make_unique<BlockingApplicationPathWatcher>(state); });
    const auto createAcknowledgement = std::chrono::steady_clock::now() - createStarted;
    ASSERT_TRUE(monitor);
    EXPECT_LT(createAcknowledgement, std::chrono::milliseconds{100});
    QTRY_VERIFY_WITH_TIMEOUT(([&state] {
                                 std::lock_guard lock(state->mutex);
                                 return state->calls == 1U;
                             }()),
                             1000);
    {
        std::lock_guard lock(state->mutex);
        EXPECT_NE(state->workerThread, ownerThread);
    }

    const auto recoveryStarted = std::chrono::steady_clock::now();
    monitor.value()->requestRecovery();
    EXPECT_LT(std::chrono::steady_clock::now() - recoveryStarted, std::chrono::milliseconds{100});
    bool ownerEventHandled = false;
    QTimer::singleShot(0, [&ownerEventHandled] { ownerEventHandled = true; });
    QTRY_VERIFY_WITH_TIMEOUT(ownerEventHandled, 1000);

    const auto destroyStarted = std::chrono::steady_clock::now();
    monitor.value().reset();
    EXPECT_LT(std::chrono::steady_clock::now() - destroyStarted, std::chrono::milliseconds{100});
    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->stopObserved);
}

TEST(ApplicationRootMonitorTest, BlockedCollectionKeepsOwnerEventLoopResponsiveAndStopsSafely) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("applications")).toStdString();
    ASSERT_TRUE(QDir{}.mkpath(QString::fromStdString(root)));
    auto state = std::make_shared<BlockingCollectorState>();
    state->results.push_back({{{root}}, false});
    const auto createStarted = std::chrono::steady_clock::now();
    auto monitor = ApplicationRootMonitor::create(
        {{{root}}}, {8U, std::chrono::milliseconds{10}, std::chrono::milliseconds{10}}, nullptr,
        std::make_unique<BlockingWatchCandidateCollector>(state));
    const auto createAcknowledgement = std::chrono::steady_clock::now() - createStarted;
    ASSERT_TRUE(monitor);
    EXPECT_LT(createAcknowledgement, std::chrono::milliseconds{100});
    QTRY_COMPARE_WITH_TIMEOUT(collectorCalls(state), 1U, 1000);

    bool ownerEventHandled = false;
    QTimer::singleShot(0, [&ownerEventHandled] { ownerEventHandled = true; });
    QTRY_VERIFY_WITH_TIMEOUT(ownerEventHandled, 1000);
    EXPECT_EQ(monitor.value()->watchedPathCount(), 0);

    monitor.value().reset();
    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->stopObserved);
}

TEST(ApplicationRootMonitorTest, DiscardsBlockedStaleCollectionBeforeMutatingOwnerWatcher) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("applications")).toStdString();
    ASSERT_TRUE(QDir{}.mkpath(QString::fromStdString(root)));
    auto state = std::make_shared<BlockingCollectorState>();
    state->results = {{{{root}}, true}, {{{root}}, false}};
    auto monitor = ApplicationRootMonitor::create(
        {{{root}}}, {8U, std::chrono::milliseconds{10}, std::chrono::milliseconds{10}}, nullptr,
        std::make_unique<BlockingWatchCandidateCollector>(state));
    ASSERT_TRUE(monitor);
    QSignalSpy degraded(monitor.value().get(), &ApplicationRootMonitor::degradedChanged);
    QTRY_COMPARE_WITH_TIMEOUT(collectorCalls(state), 1U, 1000);

    monitor.value()->requestRecovery();
    releaseCollectorCall(state, 1U);
    QTRY_COMPARE_WITH_TIMEOUT(collectorCalls(state), 2U, 1000);
    QCoreApplication::processEvents();
    EXPECT_FALSE(monitor.value()->degraded());
    EXPECT_EQ(monitor.value()->watchedPathCount(), 0);
    EXPECT_EQ(degraded.count(), 0);

    releaseCollectorCall(state, 2U);
    QTRY_COMPARE_WITH_TIMEOUT(monitor.value()->watchedPathCount(), 1, 1000);
    EXPECT_FALSE(monitor.value()->degraded());
    EXPECT_EQ(degraded.count(), 0);
}

TEST(ApplicationRootMonitorTest, PerformsExactlyThreeAutomaticRecoveryAttempts) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("applications")).toStdString();
    ASSERT_TRUE(QDir{}.mkpath(QString::fromStdString(root)));
    auto collector = std::make_unique<DegradedWatchCandidateCollector>(root);
    auto *collectorPointer = collector.get();
    auto monitor = ApplicationRootMonitor::create(
        {{{root}}}, {8U, std::chrono::milliseconds{10}, std::chrono::milliseconds{5}}, nullptr,
        std::move(collector));
    ASSERT_TRUE(monitor);

    QTRY_COMPARE_WITH_TIMEOUT(collectorPointer->calls.load(), 4U, 1000);
    QTest::qWait(150);
    EXPECT_EQ(collectorPointer->calls.load(), 4U);
    EXPECT_TRUE(monitor.value()->degraded());
}

TEST(LauncherControllerTest, MonitorMutationStartsAReplacementCatalogGeneration) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("applications"));
    ASSERT_TRUE(QDir{}.mkpath(root));
    auto index = std::make_unique<FakeIndexBackend>();
    auto launch = std::make_unique<FakeLaunchBackend>();
    auto *indexPointer = index.get();
    auto created = LauncherController::create(options(root.toStdString(), true), std::move(index),
                                              std::move(launch));
    ASSERT_TRUE(created);
    ASSERT_TRUE(created.value()->refresh());
    QTRY_COMPARE_WITH_TIMEOUT(indexPointer->refresh_calls.load(), 1, 1000);

    QFile desktop{root + QStringLiteral("/new.desktop")};
    ASSERT_TRUE(desktop.open(QIODevice::WriteOnly));
    desktop.write("[Desktop Entry]\n");
    desktop.close();
    QTRY_COMPARE_WITH_TIMEOUT(indexPointer->refresh_calls.load(), 2, 1500);
}

TEST(LauncherControllerTest, RefreshAndSearchRunOffOwnerThreadAndPublishCurrentResults) {
    auto fixture = makeController();
    const auto ownerThread = std::this_thread::get_id();
    publishInitial(fixture);

    EXPECT_NE(fixture.index->refresh_thread, ownerThread);
    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("beta")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 1, 2000);
    EXPECT_EQ(fixture.controller->presentationModel()->resultAt(0)->name(), QStringLiteral("Beta"));
}

TEST(LauncherControllerTest, InitialRefreshFailurePublishesErrorAndLaterRefreshRecovers) {
    auto fixture = makeController();
    fixture.index->fail_refresh = true;
    QSignalSpy failed(fixture.controller.get(), &LauncherController::catalogRefreshFailed);

    ASSERT_TRUE(fixture.controller->refresh());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
    EXPECT_EQ(fixture.controller->presentationModel()->viewState(),
              LauncherPresentationModel::ViewState::error);
    EXPECT_EQ(fixture.controller->presentationModel()->rowCount(), 0);

    ASSERT_TRUE(fixture.controller->refresh());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 2, 2000);
    EXPECT_EQ(fixture.controller->presentationModel()->viewState(),
              LauncherPresentationModel::ViewState::results);
}

TEST(LauncherControllerTest, ReplacementCancelsSearchAndDropsItsStaleGeneration) {
    auto fixture = makeController();
    publishInitial(fixture);
    fixture.index->hold_search = true;
    fixture.index->search_started = false;
    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("alpha")));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.index->search_started.load(), 2000);
    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("beta")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 1, 2000);
    EXPECT_EQ(fixture.controller->presentationModel()->resultAt(0)->name(), QStringLiteral("Beta"));
    EXPECT_EQ(fixture.index->cancelled_searches.load(), 1);
}

TEST(LauncherControllerTest, IgnoredCancellationCannotPublishAStaleCompletion) {
    auto fixture = makeController();
    publishInitial(fixture);
    fixture.index->hold_search = true;
    fixture.index->ignore_cancellation = true;
    fixture.index->release_search = false;
    fixture.index->search_started = false;
    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("alpha")));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.index->search_started.load(), 2000);
    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("beta")));
    fixture.index->release_search = true;
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 1, 2000);
    EXPECT_EQ(fixture.controller->presentationModel()->resultAt(0)->name(), QStringLiteral("Beta"));
}

TEST(LauncherControllerTest, InvalidQueriesAndStaleLaunchIntentsAreRejected) {
    auto fixture = makeController();
    publishInitial(fixture);
    const QString invalid = QString::fromUtf8("bad\x01", 4);
    auto rejectedQuery = fixture.controller->setSearchQuery(invalid);
    ASSERT_FALSE(rejectedQuery);
    EXPECT_EQ(rejectedQuery.error().code, ErrorCode::validation_error);

    QSignalSpy rejected(fixture.controller.get(), &LauncherController::launchRejected);
    auto identifier = prismdrake::launcher::deriveDesktopFileId("alpha.desktop");
    ASSERT_TRUE(identifier);
    emit fixture.controller->presentationModel()->launchRequested(
        {std::move(identifier).value(), 999U, 999U});
    QCOMPARE(rejected.count(), 1);
    EXPECT_EQ(fixture.launch->calls.load(), 0);
}

TEST(LauncherControllerTest, CurrentTypedIntentUsesBackgroundSafeLaunchBackend) {
    auto fixture = makeController();
    publishInitial(fixture);
    QSignalSpy completed(fixture.controller.get(), &LauncherController::launchCompleted);

    ASSERT_TRUE(fixture.controller->presentationModel()->resultAt(0)->requestLaunch());
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 2000);
    EXPECT_EQ(fixture.launch->calls.load(), 1);
    ASSERT_TRUE(fixture.launch->captured_exec);
    EXPECT_EQ(*fixture.launch->captured_exec, "/bin/echo literal");
}

TEST(LauncherControllerTest, RoutesActionPinDestinationRefreshAndResolvedIconIntents) {
    auto fixture = makeController();
    publishInitial(fixture);
    auto *alpha = fixture.controller->presentationModel()->resultAt(0);
    ASSERT_NE(alpha, nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(alpha->iconSource().scheme(), QStringLiteral("data"), 2000);

    const auto actions = alpha->actions();
    ASSERT_EQ(actions.size(), 1);
    auto *action = qobject_cast<LauncherActionPresentation *>(actions[0].value<QObject *>());
    ASSERT_NE(action, nullptr);
    QSignalSpy launched(fixture.controller.get(), &LauncherController::launchCompleted);
    ASSERT_TRUE(action->request());
    QTRY_COMPARE_WITH_TIMEOUT(launched.count(), 1, 2000);
    ASSERT_TRUE(fixture.launch->captured_action);
    EXPECT_EQ(*fixture.launch->captured_action, "NewWindow");

    std::optional<ApplicationPinIntent> pin;
    QObject::connect(fixture.controller.get(), &LauncherController::pinChangeRequested,
                     fixture.controller.get(), [&](const auto &intent) { pin = intent; });
    ASSERT_TRUE(alpha->requestPinToggle());
    ASSERT_TRUE(pin);
    EXPECT_TRUE(pin->pin);
    auto identifier = prismdrake::launcher::deriveDesktopFileId("alpha.desktop");
    ASSERT_TRUE(identifier);
    ASSERT_TRUE(fixture.controller->applyPinSnapshot(2U, {identifier.value()}));
    EXPECT_TRUE(alpha->pinned());

    const int refreshCount = fixture.index->refresh_calls.load();
    ASSERT_TRUE(fixture.controller->presentationModel()->requestRefresh());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.index->refresh_calls.load(), refreshCount + 1, 2000);

    ASSERT_TRUE(fixture.controller->setSearchQuery(QStringLiteral("notification center")));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.controller->presentationModel()->rowCount(), 1, 2000);
    auto *destination = fixture.controller->presentationModel()->resultAt(0);
    ASSERT_NE(destination, nullptr);
    EXPECT_EQ(destination->kindId(), QStringLiteral("destination"));
    std::optional<ShellDestinationIntent> shellDestination;
    QObject::connect(fixture.controller.get(), &LauncherController::shellDestinationRequested,
                     fixture.controller.get(),
                     [&](const auto &intent) { shellDestination = intent; });
    ASSERT_TRUE(destination->requestLaunch());
    ASSERT_TRUE(shellDestination);
    EXPECT_EQ(shellDestination->destination, ShellDestination::notificationCenter);
}

TEST(LauncherControllerTest, DefaultLaunchBackendBuildsLiteralArgvWithoutImplicitShell) {
    auto executor = std::make_unique<CapturingExecutor>();
    auto *capture = executor.get();
    auto backend = makeDefaultLauncherLaunchBackend(std::move(executor));
    auto application = entry("literal.desktop", "Literal",
                             "/bin/echo \"; touch /tmp/prismdrake-must-not-exist &\" %k");
    foundation::CancellationSource cancellation;

    auto launched =
        backend->launch(application, std::nullopt, options().launchContext, cancellation.token());

    ASSERT_TRUE(launched) << (launched ? "" : launched.error().message);
    ASSERT_EQ(capture->plans.size(), 1U);
    EXPECT_EQ(capture->plans[0].executable, std::filesystem::path{"/bin/echo"});
    EXPECT_EQ(capture->plans[0].argv,
              (std::vector<std::string>{"/bin/echo", "; touch /tmp/prismdrake-must-not-exist &",
                                        "/fixture/applications/literal.desktop"}));
}

TEST(LauncherControllerTest, DefaultLaunchBackendRejectsMismatchedRetainedLocation) {
    auto executor = std::make_unique<CapturingExecutor>();
    auto *capture = executor.get();
    auto backend = makeDefaultLauncherLaunchBackend(std::move(executor));
    auto application = entry("literal.desktop", "Literal", "/bin/echo %k");
    auto otherLocation = prismdrake::launcher::makeDiscoveredDesktopFileLocation(
        "/fixture/applications", "other.desktop", 0U);
    ASSERT_TRUE(otherLocation);
    application.location = std::move(otherLocation).value();
    foundation::CancellationSource cancellation;

    auto launched =
        backend->launch(application, std::nullopt, options().launchContext, cancellation.token());

    ASSERT_FALSE(launched);
    EXPECT_EQ(launched.error().code, ErrorCode::validation_error);
    EXPECT_TRUE(capture->plans.empty());
}

TEST(LauncherControllerTest, DerivesStandardDbusIdentityAndRejectsNonBusDesktopIds) {
    auto valid = prismdrake::launcher::deriveDesktopFileId("org.example.Dragon-Editor.desktop");
    ASSERT_TRUE(valid);
    auto identity = prismdrake::launcher::deriveDbusApplicationIdentity(valid.value());
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity.value().serviceName, "org.example.Dragon-Editor");
    EXPECT_EQ(identity.value().objectPath, "/org/example/Dragon_Editor");

    auto invalid = prismdrake::launcher::deriveDesktopFileId("dragon.desktop");
    ASSERT_TRUE(invalid);
    auto rejected = prismdrake::launcher::deriveDbusApplicationIdentity(invalid.value());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::unsupported);
}

TEST(LauncherControllerTest, DbusActivationUsesTypedActionAndPermittedDirectFallback) {
    auto executor = std::make_unique<CapturingExecutor>();
    auto *capture = executor.get();
    auto activator = std::make_unique<FakeApplicationActivator>();
    auto *activation = activator.get();
    auto backend = makeDefaultLauncherLaunchBackend(std::move(executor), std::move(activator));
    auto application = entry("org.example.Editor.desktop", "Editor", "/bin/echo fallback");
    application.entry.dbusActivatable = true;
    application.entry.actions.push_back(
        {"NewWindow", "New Window", std::nullopt, "/bin/echo action-fallback"});
    foundation::CancellationSource cancellation;

    auto activated = backend->launch(application, std::optional<std::string>{"NewWindow"},
                                     options().launchContext, cancellation.token());
    ASSERT_TRUE(activated);
    EXPECT_EQ(activation->calls, 1);
    EXPECT_EQ(activation->captured_id, "org.example.Editor.desktop");
    ASSERT_TRUE(activation->captured_action);
    EXPECT_EQ(*activation->captured_action, "NewWindow");
    EXPECT_TRUE(capture->plans.empty());

    activation->fail = true;
    auto fallback = backend->launch(application, std::optional<std::string>{"NewWindow"},
                                    options().launchContext, cancellation.token());
    ASSERT_TRUE(fallback);
    ASSERT_EQ(capture->plans.size(), 1U);
    EXPECT_EQ(capture->plans[0].argv, (std::vector<std::string>{"/bin/echo", "action-fallback"}));
}

TEST(LauncherControllerTest, ActivationOnlyEntryDoesNotInventDirectFallback) {
    auto executor = std::make_unique<CapturingExecutor>();
    auto *capture = executor.get();
    auto activator = std::make_unique<FakeApplicationActivator>();
    activator->fail = true;
    auto backend = makeDefaultLauncherLaunchBackend(std::move(executor), std::move(activator));
    auto application = entry("org.example.Settings.desktop", "Settings");
    application.entry.dbusActivatable = true;
    application.entry.exec.reset();
    foundation::CancellationSource cancellation;

    auto launched =
        backend->launch(application, std::nullopt, options().launchContext, cancellation.token());

    ASSERT_FALSE(launched);
    EXPECT_TRUE(capture->plans.empty());
}

} // namespace
} // namespace prismdrake::shell::launcher::controller
