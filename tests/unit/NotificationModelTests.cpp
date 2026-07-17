#include "NotificationModel.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::notifications {
namespace {

using namespace std::chrono_literals;
using foundation::Error;
using foundation::ErrorCode;
using foundation::MonotonicClock;
using foundation::TestMonotonicClock;

[[nodiscard]] SyntheticNotificationInput notification(std::string summary = "Summary") {
    SyntheticNotificationInput input;
    input.summary = std::move(summary);
    input.body = "Body";
    input.applicationName = "Fixture App";
    input.applicationId = "org.prismdrake.fixture";
    return input;
}

[[nodiscard]] SyntheticNotificationModel
model(const std::shared_ptr<TestMonotonicClock> &clock,
      NotificationModelConfig config = NotificationModelConfig{}) {
    auto created = SyntheticNotificationModel::create(clock, config);
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return std::move(created).value();
}

void expectRedacted(const Error &error, std::string_view privateText) {
    EXPECT_EQ(error.message.find(privateText), std::string::npos);
    EXPECT_EQ(error.recovery.find(privateText), std::string::npos);
}

TEST(NotificationModelTest, PublishesTypedAccessiblePlainTextCardAndFocusOrder) {
    const auto start = MonotonicClock::TimePoint{100s};
    auto clock = std::make_shared<TestMonotonicClock>(start);
    auto notifications = model(clock);
    auto input = notification("<b>Literal summary</b>");
    input.body = "Literal <script>text</script> & markup-like content";
    input.urgency = NotificationUrgency::critical;
    input.visual = NotificationThemeIcon{"dialog-information"};
    input.actions = {
        {"open", "Open", true}, {"disabled", "Unavailable", false}, {"details", "Details", true}};

    const auto published = notifications.upsert(input);

    ASSERT_TRUE(published) << (published ? "" : published.error().message);
    EXPECT_FALSE(published.value().replaced);
    EXPECT_NE(published.value().id.value(), 0U);
    ASSERT_EQ(published.value().snapshot->cards.size(), 1U);
    EXPECT_EQ(notifications.current(), published.value().snapshot);
    const auto &card = published.value().snapshot->cards.front();
    EXPECT_EQ(card.id, published.value().id);
    EXPECT_EQ(card.summary, input.summary);
    EXPECT_EQ(card.body, input.body);
    EXPECT_EQ(card.applicationName, input.applicationName);
    EXPECT_EQ(card.applicationId, input.applicationId);
    EXPECT_EQ(card.urgency, NotificationUrgency::critical);
    EXPECT_EQ(card.visual, input.visual);
    EXPECT_EQ(card.actions, input.actions);
    EXPECT_TRUE(card.focusable);
    EXPECT_TRUE(card.dismissible);
    EXPECT_EQ(card.accessibleRole, NotificationAccessibleRole::notification);
    EXPECT_EQ(card.firstPresentedAt, start);
    EXPECT_EQ(card.lastUpdatedAt, start);
    EXPECT_EQ(card.expiresAt, start + 5s);

    const auto targets = notifications.focusTargets(card.id);
    ASSERT_TRUE(targets);
    EXPECT_EQ(targets.value(),
              (std::vector<NotificationFocusTarget>{
                  {NotificationFocusTargetKind::card, NotificationAccessibleRole::notification,
                   std::nullopt},
                  {NotificationFocusTargetKind::action, NotificationAccessibleRole::button, 0U},
                  {NotificationFocusTargetKind::action, NotificationAccessibleRole::button, 2U},
                  {NotificationFocusTargetKind::dismiss, NotificationAccessibleRole::button,
                   std::nullopt}}));

    const auto activated = notifications.activateAction(card.id, card.contentGeneration, "details");
    ASSERT_TRUE(activated);
    EXPECT_EQ(activated.value(),
              (NotificationActionActivation{card.id, card.contentGeneration, "details"}));
    EXPECT_EQ(notifications.current(), published.value().snapshot);
}

TEST(NotificationModelTest, ReplacementPreservesIdentityOrderAndImmutablePriorSnapshot) {
    auto clock = std::make_shared<TestMonotonicClock>(MonotonicClock::TimePoint{10s});
    auto notifications = model(clock);
    auto firstInput = notification("First");
    firstInput.actions = {{"open", "Open", true}};
    const auto first = notifications.upsert(std::move(firstInput));
    const auto second = notifications.upsert(notification("Second"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto prior = second.value().snapshot;
    ASSERT_EQ(prior->cards.size(), 2U);
    ASSERT_TRUE(clock->advance(2s));

    auto replacement = notification("First replaced");
    replacement.replacementId = first.value().id;
    replacement.timeout = {NotificationTimeoutKind::explicitTimeout, 2500ms};
    replacement.actions = {{"open", "Open replaced", true}};
    const auto replaced = notifications.upsert(std::move(replacement));

    ASSERT_TRUE(replaced) << (replaced ? "" : replaced.error().message);
    EXPECT_TRUE(replaced.value().replaced);
    EXPECT_EQ(replaced.value().id, first.value().id);
    ASSERT_EQ(replaced.value().snapshot->cards.size(), 2U);
    EXPECT_EQ(replaced.value().snapshot->generation.value(), prior->generation.value() + 1U);
    EXPECT_EQ(replaced.value().snapshot->cards[0].id, first.value().id);
    EXPECT_EQ(replaced.value().snapshot->cards[1].id, second.value().id);
    EXPECT_EQ(replaced.value().snapshot->cards[0].summary, "First replaced");
    EXPECT_EQ(replaced.value().snapshot->cards[0].firstPresentedAt,
              prior->cards[0].firstPresentedAt);
    EXPECT_EQ(replaced.value().snapshot->cards[0].lastUpdatedAt, clock->now());
    EXPECT_EQ(replaced.value().snapshot->cards[0].expiresAt, clock->now() + 2500ms);
    EXPECT_EQ(prior->cards[0].summary, "First");
    EXPECT_EQ(prior->cards[0].lastUpdatedAt, MonotonicClock::TimePoint{10s});

    const auto staleAction =
        notifications.activateAction(first.value().id, prior->cards[0].contentGeneration, "open");
    ASSERT_FALSE(staleAction);
    EXPECT_EQ(staleAction.error().code, ErrorCode::cancelled);
    const auto currentAction = notifications.activateAction(
        first.value().id, replaced.value().snapshot->cards[0].contentGeneration, "open");
    ASSERT_TRUE(currentAction);
    EXPECT_EQ(currentAction.value().contentGeneration,
              replaced.value().snapshot->cards[0].contentGeneration);

    const auto staleDismiss =
        notifications.dismiss(first.value().id, prior->cards[0].contentGeneration);
    ASSERT_FALSE(staleDismiss);
    EXPECT_EQ(staleDismiss.error().code, ErrorCode::cancelled);
    EXPECT_EQ(notifications.current(), replaced.value().snapshot);
}

TEST(NotificationModelTest, DefaultExplicitAndNeverTimeoutsUseInclusiveInjectedDeadlines) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock, NotificationModelConfig{100ms});
    const auto defaultCard = notifications.upsert(notification("Default"));
    auto neverInput = notification("Never");
    neverInput.timeout = {NotificationTimeoutKind::never, 0ms};
    const auto neverCard = notifications.upsert(std::move(neverInput));
    auto explicitInput = notification("Explicit");
    explicitInput.timeout = {NotificationTimeoutKind::explicitTimeout, 50ms};
    const auto explicitCard = notifications.upsert(std::move(explicitInput));
    ASSERT_TRUE(defaultCard);
    ASSERT_TRUE(neverCard);
    ASSERT_TRUE(explicitCard);
    const auto allCards = explicitCard.value().snapshot;

    ASSERT_TRUE(clock->advance(49ms));
    const auto before = notifications.advanceTimeouts();
    ASSERT_TRUE(before);
    EXPECT_FALSE(before.value().published);
    EXPECT_TRUE(before.value().removedIds.empty());
    EXPECT_EQ(before.value().snapshot, allCards);

    ASSERT_TRUE(clock->advance(1ms));
    const auto explicitDeadline = notifications.advanceTimeouts();
    ASSERT_TRUE(explicitDeadline);
    EXPECT_TRUE(explicitDeadline.value().published);
    EXPECT_EQ(explicitDeadline.value().removedIds,
              (std::vector<NotificationId>{explicitCard.value().id}));

    ASSERT_TRUE(clock->advance(50ms));
    const auto defaultDeadline = notifications.advanceTimeouts();
    ASSERT_TRUE(defaultDeadline);
    EXPECT_EQ(defaultDeadline.value().removedIds,
              (std::vector<NotificationId>{defaultCard.value().id}));
    ASSERT_EQ(defaultDeadline.value().snapshot->cards.size(), 1U);
    EXPECT_EQ(defaultDeadline.value().snapshot->cards.front().id, neverCard.value().id);
    EXPECT_FALSE(defaultDeadline.value().snapshot->cards.front().expiresAt);
}

TEST(NotificationModelTest, NondismissibleCardsOmitUserDismissFocusButRemainOwnerRemovable) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    auto input = notification();
    input.actions = {{"enabled", "Enabled", true}, {"disabled", "Disabled", false}};
    input.dismissible = false;
    const auto published = notifications.upsert(std::move(input));
    ASSERT_TRUE(published);
    const auto id = published.value().id;

    const auto targets = notifications.focusTargets(id);
    ASSERT_TRUE(targets);
    EXPECT_EQ(targets.value().size(), 2U);
    EXPECT_EQ(targets.value().front().kind, NotificationFocusTargetKind::card);
    EXPECT_EQ(targets.value().back().kind, NotificationFocusTargetKind::action);
    EXPECT_EQ(targets.value().back().actionIndex, 0U);
    const auto contentGeneration = published.value().snapshot->cards.front().contentGeneration;
    EXPECT_TRUE(notifications.activateAction(id, contentGeneration, "enabled"));
    EXPECT_FALSE(notifications.activateAction(id, contentGeneration, "disabled"));
    EXPECT_FALSE(notifications.activateAction(id, contentGeneration, "missing"));
    const auto dismissed = notifications.dismiss(id, contentGeneration);
    ASSERT_TRUE(dismissed);
    EXPECT_TRUE(dismissed.value().published);
    EXPECT_TRUE(dismissed.value().snapshot->cards.empty());
}

TEST(NotificationModelTest, DismissPublishesRemovalAndRejectsStaleIdentity) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const auto published = notifications.upsert(notification());
    ASSERT_TRUE(published);
    const auto prior = published.value().snapshot;

    const auto dismissed =
        notifications.dismiss(published.value().id, prior->cards.front().contentGeneration);

    ASSERT_TRUE(dismissed);
    EXPECT_TRUE(dismissed.value().published);
    EXPECT_EQ(dismissed.value().removedIds, (std::vector<NotificationId>{published.value().id}));
    EXPECT_TRUE(dismissed.value().snapshot->cards.empty());
    EXPECT_EQ(dismissed.value().snapshot->generation.value(), prior->generation.value() + 1U);
    EXPECT_EQ(prior->cards.size(), 1U);
    EXPECT_FALSE(
        notifications.dismiss(published.value().id, prior->cards.front().contentGeneration));
    EXPECT_FALSE(notifications.focusTargets(published.value().id));
}

TEST(NotificationModelTest, AcceptsBoundedPackedRgbAndRgbaImages) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const SyntheticNotificationImage rgbImage{
        NotificationPixelFormat::rgb8, 2U, 1U, 6U, {1U, 2U, 3U, 4U, 5U, 6U}};
    auto rgb = notification("RGB");
    rgb.visual = rgbImage;
    const auto rgbResult = notifications.upsert(std::move(rgb));
    ASSERT_TRUE(rgbResult);

    const SyntheticNotificationImage rgbaImage{
        NotificationPixelFormat::rgba8, 1U, 2U, 4U, {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}};
    auto rgba = notification("RGBA");
    rgba.visual = rgbaImage;
    const auto rgbaResult = notifications.upsert(std::move(rgba));
    ASSERT_TRUE(rgbaResult);
    EXPECT_EQ(rgbaResult.value().snapshot->cards[0].visual, NotificationVisual{rgbImage});
    EXPECT_EQ(rgbaResult.value().snapshot->cards[1].visual, NotificationVisual{rgbaImage});
}

TEST(NotificationModelTest, RejectsMalformedOversizedAndControlTextWithRedactedErrors) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const auto expectRejected = [&notifications](SyntheticNotificationInput candidate,
                                                 ErrorCode code, std::string_view sentinel = {}) {
        const auto result = notifications.upsert(std::move(candidate));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code);
        if (!sentinel.empty()) {
            expectRedacted(result.error(), sentinel);
        }
        EXPECT_FALSE(notifications.current());
    };

    expectRejected(notification(""), ErrorCode::validation_error);
    expectRejected(notification(std::string(maximumNotificationSummaryBytes + 1U, 's')),
                   ErrorCode::too_large, "ssss");
    auto oversizedBody = notification();
    oversizedBody.body.assign(maximumNotificationBodyBytes + 1U, 'b');
    expectRejected(std::move(oversizedBody), ErrorCode::too_large, "bbbb");
    auto malformedUtf8 = notification(std::string{"private\xC3\x28", 9U});
    expectRejected(std::move(malformedUtf8), ErrorCode::validation_error, "private");
    auto control = notification("private-control-sentinel");
    control.body = std::string{"private\0body", 12U};
    expectRejected(std::move(control), ErrorCode::validation_error, "private");
    auto oversizedApplication = notification();
    oversizedApplication.applicationName =
        std::string(maximumNotificationApplicationNameBytes + 1U, 'a');
    expectRejected(std::move(oversizedApplication), ErrorCode::too_large, "aaaa");
    expectRejected(notification(std::string(maximumNotificationSummaryCodepoints + 1U, 'c')),
                   ErrorCode::too_large, "cccc");
    auto excessiveBodyCodepoints = notification();
    excessiveBodyCodepoints.body.assign(maximumNotificationBodyCodepoints + 1U, 'd');
    expectRejected(std::move(excessiveBodyCodepoints), ErrorCode::too_large, "dddd");
}

TEST(NotificationModelTest, RevalidatesCopiedPublishedSnapshotsAtPresentationBoundaries) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    auto source = notification("Valid published card");
    source.applicationId = "org.prismdrake.fixture";
    source.visual = NotificationThemeIcon{"dialog-information"};
    const auto published = notifications.upsert(std::move(source));
    ASSERT_TRUE(published);
    ASSERT_TRUE(validateNotificationSnapshot(*published.value().snapshot));

    auto malformedTextCards = published.value().snapshot->cards;
    malformedTextCards.front().summary = std::string{"bad\xC3\x28", 5U};
    const auto malformedText = validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(malformedTextCards)});
    ASSERT_FALSE(malformedText);
    EXPECT_EQ(malformedText.error().code, ErrorCode::validation_error);

    auto invalidIdentifierCards = published.value().snapshot->cards;
    invalidIdentifierCards.front().applicationId = "file:///private";
    EXPECT_FALSE(validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(invalidIdentifierCards)}));

    auto invalidVisualCards = published.value().snapshot->cards;
    invalidVisualCards.front().visual = SyntheticNotificationImage{
        NotificationPixelFormat::rgba8, 2U, 2U, 7U, std::vector<std::uint8_t>(16U)};
    EXPECT_FALSE(validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(invalidVisualCards)}));

    auto invalidStateCards = published.value().snapshot->cards;
    invalidStateCards.front().focusable = false;
    EXPECT_FALSE(validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(invalidStateCards)}));

    auto immediateExpiryCards = published.value().snapshot->cards;
    immediateExpiryCards.front().expiresAt = immediateExpiryCards.front().lastUpdatedAt;
    EXPECT_FALSE(validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(immediateExpiryCards)}));

    auto oversizedExpiryCards = published.value().snapshot->cards;
    oversizedExpiryCards.front().expiresAt =
        oversizedExpiryCards.front().lastUpdatedAt + maximumNotificationTimeout + 1ms;
    EXPECT_FALSE(validateNotificationSnapshot(NotificationSnapshot{
        published.value().snapshot->generation, std::move(oversizedExpiryCards)}));

    auto duplicateCards = published.value().snapshot->cards;
    duplicateCards.push_back(duplicateCards.front());
    EXPECT_FALSE(validateNotificationSnapshot(
        NotificationSnapshot{published.value().snapshot->generation, std::move(duplicateCards)}));
}

TEST(NotificationModelTest, RejectsMalformedDuplicateAndOversizedActions) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const auto expectRejected = [&notifications](std::vector<SyntheticNotificationAction> actions,
                                                 ErrorCode code) {
        auto input = notification();
        input.actions = std::move(actions);
        const auto result = notifications.upsert(std::move(input));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code);
        EXPECT_FALSE(notifications.current());
    };

    expectRejected({{"duplicate", "One", true}, {"duplicate", "Two", true}},
                   ErrorCode::validation_error);
    expectRejected({{"", "Private label", true}}, ErrorCode::validation_error);
    expectRejected({{"private-action", "", true}}, ErrorCode::validation_error);
    expectRejected(std::vector<SyntheticNotificationAction>(maximumNotificationActions + 1U,
                                                            {"id", "label", true}),
                   ErrorCode::too_large);
    expectRejected({{std::string(maximumNotificationActionIdBytes + 1U, 'i'), "label", true}},
                   ErrorCode::validation_error);
    expectRejected({{"id", std::string(maximumNotificationActionLabelBytes + 1U, 'l'), true}},
                   ErrorCode::too_large);
}

TEST(NotificationModelTest, RejectsPathUriAndUnsafeThemeIconMetadata) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    for (const std::string_view icon : {"/private/icon.png", "../private/icon", "https://private",
                                        "file:///private/icon", "name with spaces", ""}) {
        auto input = notification();
        input.visual = NotificationThemeIcon{std::string{icon}};
        const auto result = notifications.upsert(std::move(input));
        ASSERT_FALSE(result) << icon;
        EXPECT_EQ(result.error().message.find("private"), std::string::npos);
        EXPECT_EQ(result.error().recovery.find("private"), std::string::npos);
        EXPECT_FALSE(notifications.current());
    }
}

TEST(NotificationModelTest, RejectsMalformedAndOversizedPackedImageMetadata) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const auto expectRejected = [&notifications](SyntheticNotificationImage image, ErrorCode code) {
        auto input = notification();
        input.visual = std::move(image);
        const auto result = notifications.upsert(std::move(input));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, code);
        EXPECT_FALSE(notifications.current());
    };

    expectRejected({NotificationPixelFormat::rgb8, 0U, 1U, 0U, {}}, ErrorCode::validation_error);
    expectRejected(
        {NotificationPixelFormat::rgb8, maximumNotificationImageDimension + 1U, 1U, 0U, {}},
        ErrorCode::validation_error);
    expectRejected({NotificationPixelFormat::rgb8, 2U, 1U, 5U, std::vector<std::uint8_t>(5U)},
                   ErrorCode::validation_error);
    expectRejected({NotificationPixelFormat::rgba8, 1U, 2U, 4U, std::vector<std::uint8_t>(7U)},
                   ErrorCode::validation_error);
    expectRejected(
        {static_cast<NotificationPixelFormat>(255U), 1U, 1U, 4U, std::vector<std::uint8_t>(4U)},
        ErrorCode::validation_error);
    expectRejected({NotificationPixelFormat::rgba8, maximumNotificationImageDimension,
                    maximumNotificationImageDimension, maximumNotificationImageDimension * 4U,
                    std::vector<std::uint8_t>(maximumNotificationImageBytes + 1U)},
                   ErrorCode::validation_error);
}

TEST(NotificationModelTest, EnforcesAggregatePackedImageSnapshotBound) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const SyntheticNotificationImage maximumImage{
        NotificationPixelFormat::rgba8, maximumNotificationImageDimension,
        maximumNotificationImageDimension, maximumNotificationImageDimension * 4U,
        std::vector<std::uint8_t>(maximumNotificationImageBytes)};
    constexpr std::size_t acceptedImages =
        maximumNotificationSnapshotImageBytes / maximumNotificationImageBytes;
    for (std::size_t index = 0U; index < acceptedImages; ++index) {
        auto input = notification("Image " + std::to_string(index));
        input.visual = maximumImage;
        ASSERT_TRUE(notifications.upsert(std::move(input)));
    }
    const auto full = notifications.current();
    ASSERT_NE(full, nullptr);
    ASSERT_EQ(full->cards.size(), acceptedImages);

    auto overflow = notification("Private aggregate sentinel");
    overflow.visual = maximumImage;
    const auto result = notifications.upsert(std::move(overflow));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
    expectRedacted(result.error(), "Private");
    EXPECT_EQ(notifications.current(), full);
}

TEST(NotificationModelTest, RejectsInvalidTimeoutsEnumsConfigAndNullClock) {
    auto clock = std::make_shared<TestMonotonicClock>();
    EXPECT_FALSE(SyntheticNotificationModel::create(nullptr, {}));
    EXPECT_FALSE(SyntheticNotificationModel::create(clock, NotificationModelConfig{0ms}));
    EXPECT_FALSE(SyntheticNotificationModel::create(
        clock, NotificationModelConfig{std::chrono::duration_cast<std::chrono::milliseconds>(
                                           maximumNotificationTimeout) +
                                       1ms}));
    auto notifications = model(clock);

    for (const NotificationTimeout timeout :
         {NotificationTimeout{NotificationTimeoutKind::defaultTimeout, 1ms},
          NotificationTimeout{NotificationTimeoutKind::never, 1ms},
          NotificationTimeout{NotificationTimeoutKind::explicitTimeout, 0ms},
          NotificationTimeout{NotificationTimeoutKind::explicitTimeout, -1ms},
          NotificationTimeout{static_cast<NotificationTimeoutKind>(255U), 1ms}}) {
        auto input = notification();
        input.timeout = timeout;
        EXPECT_FALSE(notifications.upsert(std::move(input)));
        EXPECT_FALSE(notifications.current());
    }

    auto invalidUrgency = notification();
    invalidUrgency.urgency = static_cast<NotificationUrgency>(255U);
    EXPECT_FALSE(notifications.upsert(std::move(invalidUrgency)));

    auto nearMaximumClock = std::make_shared<TestMonotonicClock>(MonotonicClock::TimePoint::max() -
                                                                 MonotonicClock::Duration{1});
    auto nearMaximum = model(nearMaximumClock);
    const auto unrepresentableDeadline = nearMaximum.upsert(notification());
    ASSERT_FALSE(unrepresentableDeadline);
    EXPECT_EQ(unrepresentableDeadline.error().code, ErrorCode::too_large);
    EXPECT_FALSE(nearMaximum.current());
}

TEST(NotificationModelTest, EnforcesCapacityUnknownReplacementAndMovedFromState) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto notifications = model(clock);
    const auto first = notifications.upsert(notification("First"));
    ASSERT_TRUE(first);
    auto unknownReplacement = notification("Unknown replacement");
    unknownReplacement.replacementId = first.value().id;
    ASSERT_TRUE(notifications.dismiss(first.value().id,
                                      first.value().snapshot->cards.front().contentGeneration));
    EXPECT_FALSE(notifications.upsert(std::move(unknownReplacement)));

    for (std::size_t index = 0U; index < maximumNotificationCards; ++index) {
        ASSERT_TRUE(notifications.upsert(notification("Card " + std::to_string(index))));
    }
    const auto full = notifications.current();
    ASSERT_NE(full, nullptr);
    ASSERT_EQ(full->cards.size(), maximumNotificationCards);
    auto replacementAtCapacity = notification("Replacement at capacity");
    replacementAtCapacity.replacementId = full->cards.front().id;
    const auto replaced = notifications.upsert(std::move(replacementAtCapacity));
    ASSERT_TRUE(replaced);
    EXPECT_TRUE(replaced.value().replaced);
    EXPECT_EQ(replaced.value().snapshot->cards.size(), maximumNotificationCards);
    EXPECT_EQ(full->cards.front().summary, "Card 0");
    const auto currentFull = replaced.value().snapshot;
    const auto overflow = notifications.upsert(notification("Overflow private sentinel"));
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::too_large);
    expectRedacted(overflow.error(), "private");
    EXPECT_EQ(notifications.current(), currentFull);

    auto moved = std::move(notifications);
    EXPECT_FALSE(notifications.upsert(notification()));
    EXPECT_FALSE(notifications.dismiss(currentFull->cards.front().id,
                                       currentFull->cards.front().contentGeneration));
    EXPECT_FALSE(notifications.advanceTimeouts());
    EXPECT_FALSE(notifications.focusTargets(currentFull->cards.front().id));
    EXPECT_EQ(notifications.current(), nullptr);
    EXPECT_EQ(moved.current(), currentFull);
}

} // namespace
} // namespace prismdrake::notifications
