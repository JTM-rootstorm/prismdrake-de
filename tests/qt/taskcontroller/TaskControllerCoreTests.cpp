#include "TaskControllerCore.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::taskcontroller {
namespace {

[[nodiscard]] x11::WindowId window(std::uint32_t value) {
    return x11::WindowId::fromProtocol(value).value();
}

[[nodiscard]] x11::WindowIncarnationId incarnation(std::uint64_t value) {
    return x11::WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] x11::AtomId atom(std::uint32_t value) {
    return x11::AtomId::fromProtocol(value).value();
}

[[nodiscard]] x11::WindowMetadata metadata(std::uint32_t windowValue, std::string title,
                                           bool hidden = false) {
    return x11::WindowMetadata{
        window(windowValue),
        std::move(title),
        x11::ApplicationIdentityEvidence{x11::ApplicationIdentitySource::wmClass, std::nullopt,
                                         "Fixture", "fixture.desktop"},
        x11::WindowType::normal,
        true,
        {},
        1U,
        false,
        hidden,
        false,
        false,
        false,
        std::nullopt,
        {}};
}

[[nodiscard]] x11::DecodedTaskObservation decoded(std::uint32_t windowValue,
                                                  std::uint64_t incarnationValue,
                                                  std::optional<x11::WindowMetadata> value) {
    const bool available = value.has_value();
    return {window(windowValue), incarnation(incarnationValue), std::move(value),
            available ? "application-x-executable" : ""};
}

[[nodiscard]] x11::TaskModelObservation
observation(std::vector<x11::DecodedTaskObservation> windows,
            std::optional<std::uint32_t> active = std::nullopt) {
    std::vector<std::uint32_t> clients;
    clients.reserve(windows.size());
    for (const auto &entry : windows) {
        clients.push_back(entry.window.value());
    }
    auto authoritative = x11::buildEwmhTaskListSnapshot(
        x11::EwmhTaskListObservation{std::move(clients), std::nullopt, active});
    EXPECT_TRUE(authoritative);
    return {std::move(authoritative).value(), std::move(windows)};
}

TEST(TaskEventRefreshPlanTest, CoalescesRefreshesAndUniqueInvalidationsWithoutPollingHints) {
    const std::vector<x11::RootEvent> events{
        x11::RootGeometryHint{false},
        x11::OutputTopologyRefreshHint{false},
        x11::ClientTopologyHint{window(10U), x11::ClientTopologyChange::destroyed, false},
        x11::ClientTopologyHint{window(10U), x11::ClientTopologyChange::created, false},
        x11::ClientPropertyHint{window(20U), atom(30U), x11::RootPropertyState::newValue, false},
        x11::ProtocolErrorHint{},
    };
    const auto plan = planTaskEventRefresh(events);
    ASSERT_TRUE(plan);
    EXPECT_TRUE(plan.value().refreshRequired);
    ASSERT_EQ(plan.value().invalidatedWindows.size(), 1U);
    EXPECT_EQ(plan.value().invalidatedWindows.front(), window(10U));

    const std::vector<x11::RootEvent> outputOnly{x11::RootGeometryHint{false},
                                                 x11::OutputTopologyRefreshHint{false}};
    const auto ignored = planTaskEventRefresh(outputOnly);
    ASSERT_TRUE(ignored);
    EXPECT_FALSE(ignored.value().refreshRequired);
}

TEST(TaskEventRefreshPlanTest, RejectsAnOversizedSyntheticBatch) {
    std::vector<x11::RootEvent> events(x11::maximumRootEventsPerDrain + 1U,
                                       x11::ProtocolErrorHint{});
    const auto plan = planTaskEventRefresh(events);
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().code, foundation::ErrorCode::too_large);
}

TEST(TaskEventFollowUpTest, ConsumesEventsBufferedByHealthyRefreshAttemptThenStopsWhenEmpty) {
    EXPECT_TRUE(taskEventFollowUpRequired(true, false));
    EXPECT_FALSE(taskEventFollowUpRequired(false, false));
}

TEST(TaskEventFollowUpTest, RetainsBoundedBatchContinuationWithoutARefresh) {
    EXPECT_TRUE(taskEventFollowUpRequired(false, true));
    EXPECT_TRUE(taskEventFollowUpRequired(true, true));
}

TEST(TaskSnapshotStabilizationPolicyTest, UsesExactBoundAndResetsAfterSuccess) {
    TaskSnapshotStabilizationPolicy policy;
    ASSERT_EQ(policy.nextDelay(), taskSnapshotStabilizationDelays[0]);
    ASSERT_EQ(policy.nextDelay(), taskSnapshotStabilizationDelays[1]);
    policy.reset();
    EXPECT_FALSE(policy.exhausted());
    EXPECT_EQ(policy.scheduledAttemptCount(), 0U);

    for (const auto expected : taskSnapshotStabilizationDelays) {
        ASSERT_EQ(policy.nextDelay(), expected);
    }
    EXPECT_TRUE(policy.exhausted());
    EXPECT_FALSE(policy.nextDelay());
    EXPECT_EQ(policy.scheduledAttemptCount(), taskSnapshotStabilizationDelays.size());
    EXPECT_TRUE(policy.takeExhaustionReport());
    EXPECT_FALSE(policy.takeExhaustionReport());
}

TEST(TaskSnapshotStabilizationPolicyTest, RealEventStartsOneNewEpochAfterExhaustion) {
    TaskSnapshotStabilizationPolicy policy;
    for ([[maybe_unused]] const auto expected : taskSnapshotStabilizationDelays) {
        ASSERT_TRUE(policy.nextDelay());
    }
    ASSERT_TRUE(policy.exhausted());
    ASSERT_TRUE(policy.takeExhaustionReport());
    policy.beginRealEventEpoch();
    EXPECT_FALSE(policy.exhausted());
    EXPECT_EQ(policy.scheduledAttemptCount(), 0U);
    EXPECT_EQ(policy.nextDelay(), taskSnapshotStabilizationDelays.front());
}

TEST(TaskSnapshotStabilizationPolicyTest, CoalescesRelevantEventsWhileTimerIsPending) {
    EXPECT_TRUE(taskRefreshShouldRunImmediately(true, false));
    EXPECT_FALSE(taskRefreshShouldRunImmediately(true, true));
    EXPECT_FALSE(taskRefreshShouldRunImmediately(false, false));
    EXPECT_FALSE(taskRefreshShouldRunImmediately(false, true));
    EXPECT_TRUE(taskRequestPathCanDispatch(true, false));
    EXPECT_FALSE(taskRequestPathCanDispatch(true, true));
    EXPECT_FALSE(taskRequestPathCanDispatch(false, false));
}

TEST(TaskControllerCoreTest, PublishesPresentationAndConfirmsExactTypedActivation) {
    tasks::TaskPresentationModel presentation;
    std::optional<x11::TaskRequestState> dispatched;
    std::vector<TaskRequestUpdate> outcomes;
    TaskControllerCore core(
        presentation,
        [&dispatched](const x11::TaskRequestState &request) {
            dispatched = request;
            return foundation::Result<void>::success();
        },
        [&outcomes](const TaskRequestUpdate &update) { outcomes.push_back(update); });

    auto first =
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))}));
    ASSERT_TRUE(first);
    ASSERT_EQ(presentation.rowCount(), 1);
    ASSERT_TRUE(presentation.taskAt(0)->requestActivation());
    ASSERT_TRUE(dispatched.has_value());
    EXPECT_EQ(dispatched->window(), window(10U));
    EXPECT_EQ(dispatched->lifetime(), first.value()->tasks().front().lifetime());
    EXPECT_EQ(dispatched->issuedGeneration(), first.value()->generation());
    EXPECT_EQ(dispatched->action(), x11::TaskRequestAction::activate);
    EXPECT_EQ(core.pendingRequestCount(), 1U);
    EXPECT_TRUE(outcomes.empty());

    auto confirmed =
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))}, 10U));
    ASSERT_TRUE(confirmed);
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes.front().outcome, x11::TaskRequestOutcome::confirmed);
    EXPECT_EQ(outcomes.front().issuedGeneration, first.value()->generation());
    EXPECT_EQ(core.pendingRequestCount(), 0U);
}

TEST(TaskControllerCoreTest, RejectsDeliveryAndReportsTerminalOutcomeWithoutPendingState) {
    tasks::TaskPresentationModel presentation;
    std::vector<TaskRequestUpdate> outcomes;
    std::vector<foundation::Error> failures;
    TaskControllerCore core(
        presentation,
        [](const x11::TaskRequestState &) {
            return foundation::Result<void>::failure({foundation::ErrorCode::unsupported,
                                                      "Checked request unavailable.",
                                                      "Wait for a verified EWMH owner."});
        },
        [&outcomes](const TaskRequestUpdate &update) { outcomes.push_back(update); },
        [&failures](const foundation::Error &error) { failures.push_back(error); });
    ASSERT_TRUE(
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))})));

    ASSERT_TRUE(presentation.taskAt(0)->requestMinimization());
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes.front().outcome, x11::TaskRequestOutcome::deliveryRejected);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.front().code, foundation::ErrorCode::unsupported);
    EXPECT_EQ(core.pendingRequestCount(), 0U);
}

TEST(TaskControllerCoreTest, XidReuseTerminatesPendingIntentWithoutRedispatch) {
    tasks::TaskPresentationModel presentation;
    int dispatchCount = 0;
    std::vector<TaskRequestUpdate> outcomes;
    TaskControllerCore core(
        presentation,
        [&dispatchCount](const x11::TaskRequestState &) {
            ++dispatchCount;
            return foundation::Result<void>::success();
        },
        [&outcomes](const TaskRequestUpdate &update) { outcomes.push_back(update); });
    const auto first =
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Old"))}));
    ASSERT_TRUE(first);
    ASSERT_TRUE(presentation.taskAt(0)->requestMinimization());

    const auto reused =
        core.publishObservation(observation({decoded(10U, 101U, metadata(10U, "Replacement"))}));
    ASSERT_TRUE(reused);
    EXPECT_EQ(dispatchCount, 1);
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes.front().outcome, x11::TaskRequestOutcome::targetReplaced);
    EXPECT_EQ(core.pendingRequestCount(), 0U);
    ASSERT_EQ(presentation.rowCount(), 1);
    EXPECT_EQ(presentation.taskAt(0)->title(), QStringLiteral("Replacement"));
}

TEST(TaskControllerCoreTest, ClassifiesUnobservedDeliveryAsRefusedAtTheGenerationBound) {
    tasks::TaskPresentationModel presentation;
    std::vector<TaskRequestUpdate> outcomes;
    TaskControllerCore core(
        presentation,
        [](const x11::TaskRequestState &) { return foundation::Result<void>::success(); },
        [&outcomes](const TaskRequestUpdate &update) { outcomes.push_back(update); });
    ASSERT_TRUE(
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))})));
    ASSERT_TRUE(presentation.taskAt(0)->requestActivation());

    for (std::uint32_t generation = 0U; generation < taskRequestExpiryGenerations; ++generation) {
        ASSERT_TRUE(
            core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))})));
    }
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes.front().outcome, x11::TaskRequestOutcome::refused);
    EXPECT_EQ(core.pendingRequestCount(), 0U);
}

TEST(TaskControllerCoreTest, RejectsStaleGenerationAndDuplicatePendingIntent) {
    tasks::TaskPresentationModel presentation;
    int dispatchCount = 0;
    std::vector<foundation::Error> failures;
    TaskControllerCore core(
        presentation,
        [&dispatchCount](const x11::TaskRequestState &) {
            ++dispatchCount;
            return foundation::Result<void>::success();
        },
        {}, [&failures](const foundation::Error &error) { failures.push_back(error); });
    const auto first =
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))}));
    ASSERT_TRUE(first);
    const auto lifetime = first.value()->tasks().front().lifetime();

    ASSERT_TRUE(presentation.taskAt(0)->requestClose());
    ASSERT_TRUE(presentation.taskAt(0)->requestClose());
    EXPECT_EQ(dispatchCount, 1);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(failures.front().code, foundation::ErrorCode::cancelled);

    const auto newer =
        core.publishObservation(observation({decoded(10U, 100U, metadata(10U, "Dragon"))}));
    ASSERT_TRUE(newer);
    const auto stale =
        core.handleIntent(lifetime, first.value()->generation(), x11::TaskRequestAction::activate);
    EXPECT_FALSE(stale);
    EXPECT_EQ(stale.error().code, foundation::ErrorCode::cancelled);
}

} // namespace
} // namespace prismdrake::shell::taskcontroller
