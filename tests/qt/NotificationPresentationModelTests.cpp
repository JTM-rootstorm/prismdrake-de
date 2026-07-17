#include "NotificationPresentationModel.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QMetaProperty>
#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::notifications {
namespace {

using namespace std::chrono_literals;
using foundation::ErrorCode;
using foundation::Generation;
using foundation::TestMonotonicClock;
using prismdrake::notifications::NotificationId;
using prismdrake::notifications::NotificationSnapshot;
using prismdrake::notifications::NotificationTimeout;
using prismdrake::notifications::NotificationTimeoutKind;
using prismdrake::notifications::NotificationUrgency;
using prismdrake::notifications::SyntheticNotificationAction;
using prismdrake::notifications::SyntheticNotificationInput;
using prismdrake::notifications::SyntheticNotificationModel;

[[nodiscard]] SyntheticNotificationInput input(std::string summary) {
    SyntheticNotificationInput value;
    value.summary = std::move(summary);
    value.body = "Literal body";
    value.applicationName = "Fixture App";
    value.timeout = NotificationTimeout{NotificationTimeoutKind::never, 0ms};
    return value;
}

[[nodiscard]] SyntheticNotificationModel
syntheticModel(const std::shared_ptr<TestMonotonicClock> &clock) {
    auto created = SyntheticNotificationModel::create(clock);
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return std::move(created).value();
}

[[nodiscard]] NotificationActionPresentationModel *actions(NotificationCardPresentation *card) {
    return qobject_cast<NotificationActionPresentationModel *>(card->actions());
}

TEST(NotificationPresentationModelTest,
     MirrorsOrderedLiteralCardsWithoutNumericIdentityProperties) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    auto firstInput = input("<b>Literal first</b>");
    firstInput.body = "Literal <script>body</script> & text";
    firstInput.urgency = NotificationUrgency::critical;
    firstInput.actions = {{"open", "Open", true}, {"disabled", "Unavailable", false}};
    const auto first = synthetic.upsert(std::move(firstInput));
    const auto second = synthetic.upsert(input("Second"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    NotificationPresentationModel presentation;
    const auto applied = presentation.applySnapshot(second.value().snapshot);
    ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
    ASSERT_EQ(presentation.rowCount(), 2);
    EXPECT_EQ(presentation.roleNames(),
              (QHash<int, QByteArray>{{NotificationPresentationModel::cardObject, "card"}}));

    auto *firstCard = presentation.cardAt(0);
    auto *secondCard = presentation.cardAt(1);
    ASSERT_NE(firstCard, nullptr);
    ASSERT_NE(secondCard, nullptr);
    EXPECT_EQ(firstCard->summary(), QStringLiteral("<b>Literal first</b>"));
    EXPECT_EQ(firstCard->body(), QStringLiteral("Literal <script>body</script> & text"));
    EXPECT_EQ(firstCard->applicationName(), QStringLiteral("Fixture App"));
    EXPECT_EQ(firstCard->urgencyId(), QStringLiteral("critical"));
    EXPECT_EQ(secondCard->summary(), QStringLiteral("Second"));
    EXPECT_EQ(presentation.data(presentation.index(0), NotificationPresentationModel::cardObject)
                  .value<QObject *>(),
              firstCard);

    const QMetaObject *cardMetaObject = firstCard->metaObject();
    EXPECT_EQ(cardMetaObject->indexOfProperty("notificationId"), -1);
    EXPECT_EQ(cardMetaObject->indexOfProperty("contentGeneration"), -1);
    auto *firstActions = actions(firstCard);
    ASSERT_NE(firstActions, nullptr);
    ASSERT_EQ(firstActions->rowCount(), 2);
    EXPECT_EQ(firstActions->data(firstActions->index(0), NotificationActionPresentationModel::label)
                  .toString(),
              QStringLiteral("Open"));
    EXPECT_FALSE(
        firstActions->data(firstActions->index(1), NotificationActionPresentationModel::enabled)
            .toBool());
}

TEST(NotificationPresentationModelTest, ReplacementPreservesOrderAndEmitsCurrentTypedIntent) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    auto originalInput = input("Original");
    originalInput.actions = {{"open", "Open original", true}};
    const auto original = synthetic.upsert(std::move(originalInput));
    const auto second = synthetic.upsert(input("Second"));
    ASSERT_TRUE(original);
    ASSERT_TRUE(second);
    const auto prior = second.value().snapshot;

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(prior));

    std::uint64_t emittedNotification = 0U;
    std::uint64_t emittedGeneration = 0U;
    QString emittedAction;
    QObject::connect(
        &presentation, &NotificationPresentationModel::actionRequested, &presentation,
        [&](NotificationId notificationId, Generation generation, const QString &actionId) {
            emittedNotification = notificationId.value();
            emittedGeneration = generation.value();
            emittedAction = actionId;
        });

    auto *oldActions = actions(presentation.cardAt(0));
    ASSERT_NE(oldActions, nullptr);
    ASSERT_TRUE(oldActions->actionAt(0)->requestActivation());
    const std::uint64_t oldContentGeneration = emittedGeneration;
    EXPECT_EQ(emittedNotification, original.value().id.value());
    EXPECT_EQ(emittedAction, QStringLiteral("open"));

    auto replacement = input("Replaced");
    replacement.replacementId = original.value().id;
    replacement.actions = {{"details", "New details", true}};
    const auto replaced = synthetic.upsert(std::move(replacement));
    ASSERT_TRUE(replaced);
    ASSERT_TRUE(presentation.applySnapshot(replaced.value().snapshot));

    ASSERT_EQ(presentation.rowCount(), 2);
    EXPECT_EQ(presentation.cardAt(0)->summary(), QStringLiteral("Replaced"));
    EXPECT_EQ(presentation.cardAt(1)->summary(), QStringLiteral("Second"));
    auto *newActions = actions(presentation.cardAt(0));
    ASSERT_NE(newActions, nullptr);
    ASSERT_TRUE(newActions->actionAt(0)->requestActivation());
    EXPECT_EQ(emittedNotification, original.value().id.value());
    EXPECT_GT(emittedGeneration, oldContentGeneration);
    EXPECT_EQ(emittedAction, QStringLiteral("details"));

    const auto staleActivation = synthetic.activateAction(
        original.value().id, Generation::fromPublished(oldContentGeneration).value(), "open");
    ASSERT_FALSE(staleActivation);
    EXPECT_EQ(staleActivation.error().code, ErrorCode::cancelled);
}

TEST(NotificationPresentationModelTest, PreservesUnchangedObjectsAcrossIncrementalPublications) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto first = synthetic.upsert(input("First"));
    const auto second = synthetic.upsert(input("Second"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(second.value().snapshot));
    auto *firstObject = presentation.cardAt(0);
    auto *secondObject = presentation.cardAt(1);

    const auto third = synthetic.upsert(input("Third"));
    ASSERT_TRUE(third);
    ASSERT_TRUE(presentation.applySnapshot(third.value().snapshot));
    auto *thirdObject = presentation.cardAt(2);
    EXPECT_EQ(presentation.cardAt(0), firstObject);
    EXPECT_EQ(presentation.cardAt(1), secondObject);

    const auto removed =
        synthetic.dismiss(second.value().id, third.value().snapshot->cards[1].contentGeneration);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(presentation.applySnapshot(removed.value().snapshot));
    EXPECT_EQ(presentation.cardAt(0), firstObject);
    EXPECT_EQ(presentation.cardAt(1), thirdObject);

    auto replacement = input("First replaced");
    replacement.replacementId = first.value().id;
    const auto replaced = synthetic.upsert(std::move(replacement));
    ASSERT_TRUE(replaced);
    ASSERT_TRUE(presentation.applySnapshot(replaced.value().snapshot));
    EXPECT_NE(presentation.cardAt(0), firstObject);
    EXPECT_EQ(presentation.cardAt(1), thirdObject);
}

TEST(NotificationPresentationModelTest, DisabledActionsAndNondismissibleCardsEmitNoIntent) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    auto cardInput = input("Bound controls");
    cardInput.actions = {{"enabled", "Enabled", true}, {"disabled", "Disabled", false}};
    cardInput.dismissible = false;
    const auto published = synthetic.upsert(std::move(cardInput));
    ASSERT_TRUE(published);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(published.value().snapshot));
    int actionCount = 0;
    int dismissCount = 0;
    QObject::connect(&presentation, &NotificationPresentationModel::actionRequested, &presentation,
                     [&](NotificationId, Generation, const QString &) { ++actionCount; });
    QObject::connect(&presentation, &NotificationPresentationModel::dismissRequested, &presentation,
                     [&](NotificationId, Generation) { ++dismissCount; });

    auto *card = presentation.cardAt(0);
    ASSERT_NE(card, nullptr);
    auto *actionModel = actions(card);
    ASSERT_NE(actionModel, nullptr);
    EXPECT_FALSE(actionModel->actionAt(-1));
    EXPECT_FALSE(actionModel->actionAt(2));
    EXPECT_FALSE(actionModel->actionAt(1)->requestActivation());
    EXPECT_EQ(actionCount, 0);
    EXPECT_TRUE(actionModel->actionAt(0)->requestActivation());
    EXPECT_EQ(actionCount, 1);
    EXPECT_FALSE(card->requestDismissal());
    EXPECT_EQ(dismissCount, 0);
}

TEST(NotificationPresentationModelTest, DismissIntentCarriesStoredIdentityWithoutQmlRoundTrip) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto published = synthetic.upsert(input("Dismissible"));
    ASSERT_TRUE(published);
    const auto &sourceCard = published.value().snapshot->cards.front();

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(published.value().snapshot));
    std::uint64_t emittedNotification = 0U;
    std::uint64_t emittedGeneration = 0U;
    QObject::connect(&presentation, &NotificationPresentationModel::dismissRequested, &presentation,
                     [&](NotificationId notificationId, Generation generation) {
                         emittedNotification = notificationId.value();
                         emittedGeneration = generation.value();
                     });

    ASSERT_TRUE(presentation.cardAt(0)->requestDismissal());
    EXPECT_EQ(emittedNotification, sourceCard.id.value());
    EXPECT_EQ(emittedGeneration, sourceCard.contentGeneration.value());
}

TEST(NotificationPresentationModelTest, RejectsPresentationBoundsAndRetainsPriorSnapshot) {
    using prismdrake::notifications::maximumNotificationActionLabelBytes;
    using prismdrake::notifications::maximumNotificationActions;
    using prismdrake::notifications::maximumNotificationCards;
    using prismdrake::notifications::maximumNotificationSummaryBytes;

    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto published = synthetic.upsert(input("Valid"));
    ASSERT_TRUE(published);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(published.value().snapshot));
    const auto retained = presentation.currentSnapshot();

    auto oversizedTextCards = retained->cards;
    oversizedTextCards.front().summary.assign(maximumNotificationSummaryBytes + 1U, 's');
    auto oversizedText = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{retained->generation, std::move(oversizedTextCards)});
    const auto textResult = presentation.applySnapshot(std::move(oversizedText));
    ASSERT_FALSE(textResult);
    EXPECT_EQ(textResult.error().code, ErrorCode::too_large);
    EXPECT_EQ(presentation.currentSnapshot(), retained);

    auto oversizedActionCards = retained->cards;
    oversizedActionCards.front().actions.assign(maximumNotificationActions + 1U,
                                                SyntheticNotificationAction{"same", "Label", true});
    auto oversizedActions = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{retained->generation, std::move(oversizedActionCards)});
    const auto actionResult = presentation.applySnapshot(std::move(oversizedActions));
    ASSERT_FALSE(actionResult);
    EXPECT_EQ(actionResult.error().code, ErrorCode::too_large);
    EXPECT_EQ(presentation.currentSnapshot(), retained);

    auto oversizedLabelCards = retained->cards;
    oversizedLabelCards.front().actions = {
        {"action", std::string(maximumNotificationActionLabelBytes + 1U, 'l'), true}};
    auto oversizedLabel = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{retained->generation, std::move(oversizedLabelCards)});
    EXPECT_FALSE(presentation.applySnapshot(std::move(oversizedLabel)));

    auto tooManyCards = retained->cards;
    tooManyCards.assign(maximumNotificationCards + 1U, retained->cards.front());
    auto oversizedSnapshot = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{retained->generation, std::move(tooManyCards)});
    EXPECT_FALSE(presentation.applySnapshot(std::move(oversizedSnapshot)));
    EXPECT_EQ(presentation.rowCount(), 1);
    EXPECT_EQ(presentation.cardAt(0)->summary(), QStringLiteral("Valid"));
}

TEST(NotificationPresentationModelTest, RejectsStaleAndConflictingGenerationsWithoutReset) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto first = synthetic.upsert(input("First"));
    const auto second = synthetic.upsert(input("Second"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(second.value().snapshot));
    const auto retained = presentation.currentSnapshot();
    int reconciliationStarts = 0;
    QObject::connect(&presentation,
                     &NotificationPresentationModel::publicationReconciliationStarted,
                     &presentation, [&] { ++reconciliationStarts; });

    const auto stale = presentation.applySnapshot(first.value().snapshot);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::cancelled);
    EXPECT_EQ(presentation.currentSnapshot(), retained);

    auto conflictingCards = retained->cards;
    conflictingCards.front().body = "Different but bounded";
    auto conflicting = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{retained->generation, std::move(conflictingCards)});
    const auto conflict = presentation.applySnapshot(std::move(conflicting));
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::validation_error);
    EXPECT_EQ(presentation.currentSnapshot(), retained);

    auto unversionedCards = retained->cards;
    unversionedCards.front().body = "Different content without a new content generation";
    const auto nextGeneration = retained->generation.next();
    ASSERT_TRUE(nextGeneration);
    auto unversioned = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{nextGeneration.value(), std::move(unversionedCards)});
    const auto unversionedResult = presentation.applySnapshot(std::move(unversioned));
    ASSERT_FALSE(unversionedResult);
    EXPECT_EQ(unversionedResult.error().code, ErrorCode::validation_error);
    EXPECT_EQ(presentation.currentSnapshot(), retained);
    EXPECT_EQ(reconciliationStarts, 0);

    EXPECT_TRUE(presentation.applySnapshot(retained));
    EXPECT_FALSE(presentation.applySnapshot(nullptr));
    EXPECT_EQ(reconciliationStarts, 0);
}

TEST(NotificationPresentationModelTest, RejectsInvalidCopiedContentAndCrossThreadMutation) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto first = synthetic.upsert(input("First"));
    const auto second = synthetic.upsert(input("Second"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(first.value().snapshot));
    const auto retained = presentation.currentSnapshot();

    auto malformedCards = second.value().snapshot->cards;
    malformedCards.front().summary = std::string{"bad\xC3\x28", 5U};
    auto malformed = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{second.value().snapshot->generation, std::move(malformedCards)});
    const auto malformedResult = presentation.applySnapshot(std::move(malformed));
    ASSERT_FALSE(malformedResult);
    EXPECT_EQ(malformedResult.error().code, ErrorCode::validation_error);
    EXPECT_EQ(presentation.currentSnapshot(), retained);

    std::promise<foundation::Result<void>> resultPromise;
    auto resultFuture = resultPromise.get_future();
    std::thread worker([&presentation, &resultPromise, snapshot = second.value().snapshot] {
        resultPromise.set_value(presentation.applySnapshot(snapshot));
    });
    worker.join();
    const auto crossThread = resultFuture.get();
    ASSERT_FALSE(crossThread);
    EXPECT_EQ(crossThread.error().code, ErrorCode::cancelled);
    EXPECT_EQ(presentation.currentSnapshot(), retained);
}

TEST(NotificationPresentationModelTest, RejectsReentrantApplicationUntilRowsAreCoherent) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto synthetic = syntheticModel(clock);
    const auto first = synthetic.upsert(input("First"));
    const auto second = synthetic.upsert(input("Second"));
    const auto third = synthetic.upsert(input("Third"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);

    NotificationPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(first.value().snapshot));

    std::optional<foundation::Result<void>> nestedResult;
    bool observedApplying = false;
    QObject::connect(&presentation,
                     &NotificationPresentationModel::publicationReconciliationStarted,
                     &presentation, [&] {
                         if (nestedResult.has_value()) {
                             return;
                         }
                         observedApplying = presentation.isApplyingSnapshot();
                         nestedResult = presentation.applySnapshot(third.value().snapshot);
                     });

    int completedPublications = 0;
    QObject::connect(&presentation, &NotificationPresentationModel::publicationApplied,
                     &presentation, [&] { ++completedPublications; });
    ASSERT_TRUE(presentation.applySnapshot(second.value().snapshot));
    ASSERT_TRUE(nestedResult.has_value());
    ASSERT_FALSE(*nestedResult);
    EXPECT_EQ(nestedResult->error().code, ErrorCode::cancelled);
    EXPECT_TRUE(observedApplying);
    EXPECT_FALSE(presentation.isApplyingSnapshot());
    EXPECT_EQ(presentation.currentSnapshot(), second.value().snapshot);
    EXPECT_EQ(presentation.rowCount(), 2);
    EXPECT_EQ(completedPublications, 1);

    ASSERT_TRUE(presentation.applySnapshot(third.value().snapshot));
    EXPECT_EQ(presentation.currentSnapshot(), third.value().snapshot);
    EXPECT_EQ(presentation.rowCount(), 3);
    EXPECT_EQ(completedPublications, 2);
}

} // namespace
} // namespace prismdrake::shell::notifications
