#include "TaskRequestState.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

[[nodiscard]] WindowIncarnationId incarnation(std::uint64_t value) {
    return WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] WindowMetadata metadata(std::uint32_t windowValue, std::string title,
                                      bool hidden = false, bool skipTaskbar = false) {
    return WindowMetadata{window(windowValue),
                          std::move(title),
                          ApplicationIdentityEvidence{ApplicationIdentitySource::wmClass,
                                                      std::nullopt, "Fixture", "fixture.desktop"},
                          WindowType::normal,
                          true,
                          {},
                          1U,
                          false,
                          hidden,
                          false,
                          skipTaskbar,
                          false,
                          std::nullopt,
                          {}};
}

[[nodiscard]] DecodedTaskObservation decoded(std::uint32_t windowValue,
                                             std::uint64_t incarnationValue,
                                             std::optional<WindowMetadata> value) {
    const bool available = value.has_value();
    return {window(windowValue), incarnation(incarnationValue), std::move(value),
            available ? "application-x-executable" : ""};
}

[[nodiscard]] std::shared_ptr<const TaskModelSnapshot>
publish(TaskModel &model, std::vector<DecodedTaskObservation> observations,
        std::optional<std::uint32_t> active = std::nullopt) {
    std::vector<std::uint32_t> clients;
    clients.reserve(observations.size());
    for (const auto &observation : observations) {
        clients.push_back(observation.window.value());
    }
    auto authoritative = buildEwmhTaskListSnapshot(
        EwmhTaskListObservation{std::move(clients), std::nullopt, active});
    if (!authoritative) {
        return {};
    }
    auto snapshot = model.publish(
        TaskModelObservation{std::move(authoritative).value(), std::move(observations)});
    return snapshot ? std::move(snapshot).value() : nullptr;
}

[[nodiscard]] TaskRequestState request(const TaskModelSnapshot &snapshot, TaskRequestAction action,
                                       std::uint32_t expiry = 3U) {
    auto issued =
        TaskRequestState::issue(snapshot, snapshot.tasks().front().lifetime(), action, expiry);
    EXPECT_TRUE(issued);
    return std::move(issued).value();
}

TEST(TaskRequestStateTest, BindsCurrentLifetimeWindowIncarnationActionAndGeneration) {
    TaskModel model;
    const auto snapshot = publish(model, {decoded(10U, 100U, metadata(10U, "private title"))});
    ASSERT_NE(snapshot, nullptr);

    auto state = request(*snapshot, TaskRequestAction::activate, 9U);
    EXPECT_EQ(state.window(), window(10U));
    EXPECT_EQ(state.incarnation(), incarnation(100U));
    EXPECT_EQ(state.lifetime(), snapshot->tasks().front().lifetime());
    EXPECT_EQ(state.action(), TaskRequestAction::activate);
    EXPECT_EQ(state.issuedGeneration(), snapshot->generation());
    EXPECT_EQ(state.expiryGenerations(), 9U);
    EXPECT_EQ(state.delivery(), TaskRequestDelivery::awaitingCheck);
    EXPECT_TRUE(state.canDispatch(*snapshot));
    EXPECT_EQ(state.evaluate(*snapshot), TaskRequestOutcome::awaitingDeliveryCheck);
}

TEST(TaskRequestStateTest, RejectsInvalidBoundsActionsAndMissingTargetsWithStaticErrors) {
    TaskModel model;
    const auto first = publish(model, {decoded(10U, 100U, metadata(10U, "secret title"))});
    ASSERT_NE(first, nullptr);
    const auto lifetime = first->tasks().front().lifetime();

    const auto zero = TaskRequestState::issue(*first, lifetime, TaskRequestAction::activate, 0U);
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, foundation::ErrorCode::invalid_argument);
    const auto oversized = TaskRequestState::issue(*first, lifetime, TaskRequestAction::activate,
                                                   maximumTaskRequestExpiryGenerations + 1U);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, foundation::ErrorCode::invalid_argument);
    const auto invalidAction =
        TaskRequestState::issue(*first, lifetime, static_cast<TaskRequestAction>(255U), 1U);
    ASSERT_FALSE(invalidAction);
    EXPECT_EQ(invalidAction.error().code, foundation::ErrorCode::invalid_argument);

    const auto empty = publish(model, {});
    ASSERT_NE(empty, nullptr);
    const auto missing = TaskRequestState::issue(*empty, lifetime, TaskRequestAction::close, 1U);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, foundation::ErrorCode::not_found);
    EXPECT_EQ(missing.error().message.find("secret title"), std::string::npos);
    EXPECT_EQ(missing.error().recovery.find("secret title"), std::string::npos);
}

TEST(TaskRequestStateTest, KeepsCheckedDeliverySeparateFromObservedConfirmation) {
    TaskModel model;
    const auto snapshot = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(snapshot, nullptr);
    auto state = request(*snapshot, TaskRequestAction::activate);

    const auto invalid = state.recordCheckedDelivery(TaskRequestDelivery::awaitingCheck);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, foundation::ErrorCode::validation_error);
    ASSERT_TRUE(state.recordCheckedDelivery(TaskRequestDelivery::delivered));
    EXPECT_EQ(state.delivery(), TaskRequestDelivery::delivered);
    EXPECT_FALSE(state.canDispatch(*snapshot));
    EXPECT_EQ(state.evaluate(*snapshot), TaskRequestOutcome::awaitingNewerSnapshot);
    EXPECT_FALSE(isTerminalTaskRequestOutcome(TaskRequestOutcome::awaitingNewerSnapshot));
    EXPECT_FALSE(state.recordCheckedDelivery(TaskRequestDelivery::rejected));

    auto rejected = request(*snapshot, TaskRequestAction::minimize);
    ASSERT_TRUE(rejected.recordCheckedDelivery(TaskRequestDelivery::rejected));
    EXPECT_EQ(rejected.evaluate(*snapshot), TaskRequestOutcome::deliveryRejected);
    EXPECT_TRUE(isTerminalTaskRequestOutcome(TaskRequestOutcome::deliveryRejected));
}

TEST(TaskRequestStateTest, ConfirmsActivateAndMinimizeOnlyFromNewerMatchingLifetime) {
    TaskModel model;
    const auto initial = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(initial, nullptr);
    auto activate = request(*initial, TaskRequestAction::activate);
    auto minimize = request(*initial, TaskRequestAction::minimize);
    ASSERT_TRUE(activate.recordCheckedDelivery(TaskRequestDelivery::delivered));
    ASSERT_TRUE(minimize.recordCheckedDelivery(TaskRequestDelivery::delivered));

    const auto observed = publish(model, {decoded(10U, 100U, metadata(10U, "task", true))}, 10U);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(activate.evaluate(*observed), TaskRequestOutcome::confirmed);
    EXPECT_EQ(minimize.evaluate(*observed), TaskRequestOutcome::confirmed);
    EXPECT_TRUE(isTerminalTaskRequestOutcome(TaskRequestOutcome::confirmed));
}

TEST(TaskRequestStateTest, SameAndOlderGenerationsRemainPendingWithoutUnsignedUnderflow) {
    TaskModel model;
    const auto older = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    const auto issuedFrom = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(older, nullptr);
    ASSERT_NE(issuedFrom, nullptr);
    auto state = request(*issuedFrom, TaskRequestAction::activate, 1U);
    ASSERT_TRUE(state.recordCheckedDelivery(TaskRequestDelivery::delivered));

    EXPECT_EQ(state.evaluate(*older), TaskRequestOutcome::awaitingNewerSnapshot);
    EXPECT_EQ(state.evaluate(*issuedFrom), TaskRequestOutcome::awaitingNewerSnapshot);
}

TEST(TaskRequestStateTest, CallerBoundedExpiryClassifiesUnobservedActionAsRefused) {
    TaskModel model;
    const auto initial = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(initial, nullptr);
    auto state = request(*initial, TaskRequestAction::activate, 2U);
    ASSERT_TRUE(state.recordCheckedDelivery(TaskRequestDelivery::delivered));

    const auto one = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    const auto two = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    EXPECT_EQ(state.evaluate(*one), TaskRequestOutcome::pendingConfirmation);
    EXPECT_EQ(state.evaluate(*two), TaskRequestOutcome::refused);
    EXPECT_TRUE(isTerminalTaskRequestOutcome(TaskRequestOutcome::refused));
}

TEST(TaskRequestStateTest, CloseNeedsNewerAuthoritativeIncarnationRemoval) {
    TaskModel model;
    const auto initial = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(initial, nullptr);
    auto state = request(*initial, TaskRequestAction::close, 3U);
    ASSERT_TRUE(state.recordCheckedDelivery(TaskRequestDelivery::delivered));
    EXPECT_EQ(state.evaluate(*initial), TaskRequestOutcome::awaitingNewerSnapshot);

    const auto filtered = publish(model, {decoded(10U, 100U, metadata(10U, "task", false, true))});
    ASSERT_NE(filtered, nullptr);
    EXPECT_TRUE(filtered->tasks().empty());
    EXPECT_TRUE(filtered->authoritativelyContains(window(10U), incarnation(100U)));
    EXPECT_EQ(state.evaluate(*filtered), TaskRequestOutcome::pendingConfirmation);

    const auto removed = publish(model, {});
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(state.evaluate(*removed), TaskRequestOutcome::confirmed);
}

TEST(TaskRequestStateTest, ReusedXidNeverConfirmsOrDispatchesOldNonCloseRequest) {
    TaskModel model;
    const auto initial = publish(model, {decoded(10U, 100U, metadata(10U, "old"))});
    ASSERT_NE(initial, nullptr);
    auto activate = request(*initial, TaskRequestAction::activate);
    auto close = request(*initial, TaskRequestAction::close);
    ASSERT_TRUE(activate.recordCheckedDelivery(TaskRequestDelivery::delivered));
    ASSERT_TRUE(close.recordCheckedDelivery(TaskRequestDelivery::delivered));

    const auto reusedWithUnavailableMetadata =
        publish(model, {decoded(10U, 101U, std::nullopt)}, 10U);
    ASSERT_NE(reusedWithUnavailableMetadata, nullptr);
    EXPECT_TRUE(reusedWithUnavailableMetadata->tasks().empty());
    EXPECT_EQ(activate.evaluate(*reusedWithUnavailableMetadata),
              TaskRequestOutcome::targetReplaced);
    EXPECT_EQ(close.evaluate(*reusedWithUnavailableMetadata), TaskRequestOutcome::confirmed);
    EXPECT_FALSE(activate.canDispatch(*reusedWithUnavailableMetadata));
    EXPECT_TRUE(isTerminalTaskRequestOutcome(TaskRequestOutcome::targetReplaced));
}

TEST(TaskRequestStateTest, DispatchGuardRejectsOlderNewerReusedAndFilteredSnapshots) {
    TaskModel model;
    const auto older = publish(model, {decoded(10U, 100U, metadata(10U, "older"))});
    const auto issuedFrom = publish(model, {decoded(10U, 100U, metadata(10U, "current"))});
    ASSERT_NE(older, nullptr);
    ASSERT_NE(issuedFrom, nullptr);
    auto state = request(*issuedFrom, TaskRequestAction::minimize);
    EXPECT_TRUE(state.canDispatch(*issuedFrom));
    EXPECT_FALSE(state.canDispatch(*older));

    const auto newer = publish(model, {decoded(10U, 100U, metadata(10U, "newer"))});
    ASSERT_NE(newer, nullptr);
    EXPECT_FALSE(state.canDispatch(*newer));
    const auto filtered =
        publish(model, {decoded(10U, 100U, metadata(10U, "filtered", false, true))});
    ASSERT_NE(filtered, nullptr);
    EXPECT_FALSE(state.canDispatch(*filtered));
    const auto reused = publish(model, {decoded(10U, 101U, metadata(10U, "reused"))});
    ASSERT_NE(reused, nullptr);
    EXPECT_FALSE(state.canDispatch(*reused));
}

TEST(TaskRequestStateTest, DisappearedActivationTargetTerminatesWithoutConfirmation) {
    TaskModel model;
    const auto initial = publish(model, {decoded(10U, 100U, metadata(10U, "task"))});
    ASSERT_NE(initial, nullptr);
    auto state = request(*initial, TaskRequestAction::activate);
    ASSERT_TRUE(state.recordCheckedDelivery(TaskRequestDelivery::delivered));

    const auto removed = publish(model, {});
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(state.evaluate(*removed), TaskRequestOutcome::targetDisappeared);
    EXPECT_TRUE(isTerminalTaskRequestOutcome(TaskRequestOutcome::targetDisappeared));
}

} // namespace
} // namespace prismdrake::x11
