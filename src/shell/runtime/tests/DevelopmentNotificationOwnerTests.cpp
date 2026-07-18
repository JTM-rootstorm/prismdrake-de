#include "DevelopmentNotificationOwner.hpp"

#include <gtest/gtest.h>

#include <QString>

#include <memory>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::TestMonotonicClock;

[[nodiscard]] std::unique_ptr<DevelopmentNotificationOwner> owner() {
    auto created = DevelopmentNotificationOwner::create(std::make_shared<TestMonotonicClock>());
    if (!created) {
        return nullptr;
    }
    return std::move(created).value();
}

TEST(DevelopmentNotificationOwnerTest, PublishesAndReplacesOneBoundedTranslatedFixture) {
    auto notificationOwner = owner();
    ASSERT_NE(notificationOwner, nullptr);

    ASSERT_TRUE(notificationOwner->publishFixture());
    auto first = notificationOwner->presentation()->currentSnapshot();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->cards.size(), 1U);
    const auto &firstCard = first->cards.front();
    EXPECT_FALSE(firstCard.summary.empty());
    EXPECT_FALSE(firstCard.body.empty());
    EXPECT_EQ(firstCard.applicationId, "org.prismdrake.development-harness");
    ASSERT_EQ(firstCard.actions.size(), 1U);
    EXPECT_EQ(firstCard.actions.front().id, "acknowledge");
    EXPECT_TRUE(firstCard.actions.front().enabled);
    EXPECT_FALSE(firstCard.expiresAt.has_value());

    ASSERT_TRUE(notificationOwner->publishFixture());
    const auto second = notificationOwner->presentation()->currentSnapshot();
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(second->cards.size(), 1U);
    EXPECT_EQ(second->cards.front().id, firstCard.id);
    EXPECT_GT(second->cards.front().contentGeneration, firstCard.contentGeneration);
    EXPECT_TRUE(notificationOwner->hasCard());
}

TEST(DevelopmentNotificationOwnerTest, RejectsStaleOrUnknownActionAndAcknowledgesCurrentCard) {
    auto notificationOwner = owner();
    ASSERT_NE(notificationOwner, nullptr);
    ASSERT_TRUE(notificationOwner->publishFixture());
    const auto first = notificationOwner->presentation()->currentSnapshot();
    ASSERT_NE(first, nullptr);
    const auto staleId = first->cards.front().id;
    const auto staleGeneration = first->cards.front().contentGeneration;

    auto unknown =
        notificationOwner->activate(staleId, staleGeneration, QStringLiteral("not-an-action"));
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::not_found);
    EXPECT_TRUE(notificationOwner->hasCard());

    ASSERT_TRUE(notificationOwner->publishFixture());
    const auto current = notificationOwner->presentation()->currentSnapshot();
    ASSERT_NE(current, nullptr);
    ASSERT_EQ(current->cards.size(), 1U);
    auto stale = notificationOwner->dismiss(staleId, staleGeneration);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::cancelled);
    EXPECT_TRUE(notificationOwner->hasCard());

    ASSERT_TRUE(notificationOwner->activate(current->cards.front().id,
                                            current->cards.front().contentGeneration,
                                            QStringLiteral("acknowledge")));
    EXPECT_FALSE(notificationOwner->hasCard());
    ASSERT_NE(notificationOwner->presentation()->currentSnapshot(), nullptr);
    EXPECT_TRUE(notificationOwner->presentation()->currentSnapshot()->cards.empty());
}

TEST(DevelopmentNotificationOwnerTest, RetainsPresentationAcrossViewEpochBoundaries) {
    auto notificationOwner = owner();
    ASSERT_NE(notificationOwner, nullptr);
    auto *const presentation = notificationOwner->presentation();
    ASSERT_TRUE(notificationOwner->publishFixture());
    const auto retained = presentation->currentSnapshot();
    ASSERT_NE(retained, nullptr);

    // QML view epochs only observe this owner. Destroying and recreating those observers does not
    // clear the validated publication or replace the stable presentation-model object.
    EXPECT_EQ(notificationOwner->presentation(), presentation);
    EXPECT_EQ(notificationOwner->presentation()->currentSnapshot(), retained);
    EXPECT_EQ(notificationOwner->presentation()->rowCount(), 1);

    const auto &card = retained->cards.front();
    ASSERT_TRUE(notificationOwner->dismiss(card.id, card.contentGeneration));
    EXPECT_FALSE(notificationOwner->hasCard());
}

} // namespace
} // namespace prismdrake::shell::runtime
