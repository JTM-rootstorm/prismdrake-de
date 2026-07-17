#include "LauncherController.hpp"

#include "DesktopFileId.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
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
                                    const prismdrake::launcher::ProcessLaunchContext &,
                                    const foundation::CancellationToken &) override {
        ++calls;
        captured_exec = entry.entry.exec;
        return foundation::Result<void>::success();
    }

    std::atomic_int calls{0};
    std::optional<std::string> captured_exec;
};

class CapturingExecutor final : public LauncherProcessExecutor {
  public:
    foundation::Result<void> execute(const prismdrake::launcher::ProcessLaunchPlan &plan,
                                     const foundation::CancellationToken &) override {
        plans.push_back(plan);
        return foundation::Result<void>::success();
    }

    std::vector<prismdrake::launcher::ProcessLaunchPlan> plans;
};

[[nodiscard]] LauncherControllerOptions options() {
    auto desktop = prismdrake::launcher::parseCurrentDesktopContext("Prismdrake");
    EXPECT_TRUE(desktop);
    prismdrake::launcher::ProcessLaunchContext launch{
        {"/bin:/usr/bin", "/"}, "/tmp", {"LANG=C"}, std::nullopt};
    return {{{"/tmp/prismdrake-controller-missing-applications"}},
            {"C"},
            std::move(desktop).value(),
            {},
            {"/bin:/usr/bin", "/"},
            std::move(launch)};
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

TEST(LauncherControllerTest, DefaultLaunchBackendBuildsLiteralArgvWithoutImplicitShell) {
    auto executor = std::make_unique<CapturingExecutor>();
    auto *capture = executor.get();
    auto backend = makeDefaultLauncherLaunchBackend(std::move(executor));
    auto application = entry("literal.desktop", "Literal",
                             "/bin/echo \"; touch /tmp/prismdrake-must-not-exist &\" %k");
    foundation::CancellationSource cancellation;

    auto launched = backend->launch(application, options().launchContext, cancellation.token());

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

    auto launched = backend->launch(application, options().launchContext, cancellation.token());

    ASSERT_FALSE(launched);
    EXPECT_EQ(launched.error().code, ErrorCode::validation_error);
    EXPECT_TRUE(capture->plans.empty());
}

} // namespace
} // namespace prismdrake::shell::launcher::controller
