#include "NotificationCenterPresentation.hpp"
#include "NotificationSurfaceIntents.hpp"

#include <gtest/gtest.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::notificationcenter {
namespace {

[[nodiscard]] NotificationCenterRecord record(std::uint64_t recordId, bool transient, bool live,
                                              bool actionEnabled) {
    NotificationCenterRecord value;
    value.recordId = recordId;
    value.contentGeneration = 2U;
    value.notificationId =
        live ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(recordId)} : std::nullopt;
    value.applicationName = "Fixture";
    value.desktopFileId = "fixture.desktop";
    value.summary = "Literal <b>summary</b>";
    value.body = "Body";
    value.urgency =
        transient ? notifications::ServiceUrgency::critical : notifications::ServiceUrgency::normal;
    value.receivedAtUnixMilliseconds = 100U + recordId;
    value.dismissible = true;
    value.transientPresentation = transient;
    value.actions = {{"open", "Open", actionEnabled}};
    return value;
}

[[nodiscard]] NotificationCenterSnapshotPtr snapshot(std::uint64_t generation = 1U) {
    auto value = std::make_shared<NotificationCenterSnapshot>();
    value->generation = generation;
    value->historyMode = notifications::HistoryMode::persistent;
    value->records = {record(10U, true, true, true), record(20U, false, false, false)};
    value->serializedJson = "generation-" + std::to_string(generation);
    return value;
}

TEST(NotificationCenterPresentationTest, SeparatesHistoryAndTransientMirrors) {
    NotificationCenterPresentation presentation;
    ASSERT_TRUE(presentation.applySnapshot(snapshot()));

    EXPECT_TRUE(presentation.serviceAvailable());
    EXPECT_EQ(presentation.historyCount(), 2);
    EXPECT_EQ(presentation.historyModel()->rowCount(), 2);
    EXPECT_EQ(presentation.transientModel()->rowCount(), 1);
    EXPECT_EQ(presentation.historyModeId(), QStringLiteral("persistent"));

    auto *history = static_cast<NotificationRecordListModel *>(presentation.historyModel());
    ASSERT_NE(history->cardAt(0), nullptr);
    EXPECT_TRUE(history->cardAt(0)->live());
    EXPECT_TRUE(history->cardAt(0)->dismissible());
    EXPECT_EQ(history->cardAt(0)->summary(), QStringLiteral("Literal <b>summary</b>"));
    ASSERT_NE(history->cardAt(1), nullptr);
    EXPECT_FALSE(history->cardAt(1)->live());
    EXPECT_FALSE(history->cardAt(1)->dismissible());
}

TEST(NotificationCenterPresentationTest, KeepsPrivateIdentitiesInsideTypedIntents) {
    NotificationCenterPresentation presentation;
    ASSERT_TRUE(presentation.applySnapshot(snapshot()));
    std::uint64_t actionRecord = 0U;
    std::uint64_t actionGeneration = 0U;
    QString actionId;
    QObject::connect(&presentation, &NotificationCenterPresentation::actionRequested,
                     [&actionRecord, &actionGeneration,
                      &actionId](NotificationRecordCommandIdentity identity, const QString &id) {
                         actionRecord = identity.recordId;
                         actionGeneration = identity.contentGeneration;
                         actionId = id;
                     });

    auto *history = static_cast<NotificationRecordListModel *>(presentation.historyModel());
    auto *liveAction =
        static_cast<NotificationActionListModel *>(history->cardAt(0)->actions())->actionAt(0);
    ASSERT_NE(liveAction, nullptr);
    EXPECT_TRUE(liveAction->requestActivation());
    EXPECT_EQ(actionRecord, 10U);
    EXPECT_EQ(actionGeneration, 2U);
    EXPECT_EQ(actionId, QStringLiteral("open"));

    auto *deadAction =
        static_cast<NotificationActionListModel *>(history->cardAt(1)->actions())->actionAt(0);
    ASSERT_NE(deadAction, nullptr);
    EXPECT_FALSE(deadAction->enabled());
    EXPECT_FALSE(deadAction->requestActivation());
    EXPECT_EQ(actionRecord, 10U);
}

TEST(NotificationCenterPresentationTest, ReconcilesStableRowsAndRejectsStaleState) {
    NotificationCenterPresentation presentation;
    const auto first = snapshot(2U);
    ASSERT_TRUE(presentation.applySnapshot(first));
    auto *history = static_cast<NotificationRecordListModel *>(presentation.historyModel());
    auto *retained = history->cardAt(0);

    EXPECT_TRUE(presentation.applySnapshot(first));
    EXPECT_EQ(history->cardAt(0), retained);

    auto replacement = std::make_shared<NotificationCenterSnapshot>(*snapshot(3U));
    replacement->records.front().summary = "Replacement";
    replacement->serializedJson = "generation-3-replacement";
    ASSERT_TRUE(presentation.applySnapshot(replacement));
    EXPECT_NE(history->cardAt(0), retained);
    EXPECT_EQ(history->cardAt(0)->summary(), QStringLiteral("Replacement"));

    EXPECT_FALSE(presentation.applySnapshot(snapshot(1U)));
    EXPECT_EQ(presentation.currentSnapshot()->generation, 3U);
}

TEST(NotificationCenterPresentationTest, EmitsOnlyAvailablePolicyMutations) {
    NotificationCenterPresentation presentation;
    int dndRequests = 0;
    int clearRequests = 0;
    QObject::connect(&presentation, &NotificationCenterPresentation::doNotDisturbRequested,
                     [&dndRequests](bool enabled) { dndRequests += enabled ? 1 : 100; });
    QObject::connect(&presentation, &NotificationCenterPresentation::clearHistoryRequested,
                     [&clearRequests]() { ++clearRequests; });

    EXPECT_FALSE(presentation.requestDoNotDisturb(true));
    EXPECT_FALSE(presentation.requestClearHistory());
    ASSERT_TRUE(presentation.applySnapshot(snapshot()));
    EXPECT_TRUE(presentation.requestDoNotDisturb(true));
    EXPECT_FALSE(presentation.requestDoNotDisturb(false));
    EXPECT_TRUE(presentation.requestClearHistory());
    EXPECT_EQ(dndRequests, 1);
    EXPECT_EQ(clearRequests, 1);

    presentation.clear();
    EXPECT_FALSE(presentation.serviceAvailable());
    EXPECT_EQ(presentation.historyModel()->rowCount(), 0);
    EXPECT_EQ(presentation.transientModel()->rowCount(), 0);
}

TEST(NotificationSurfaceIntentSourceTest, ProducesBoundedTypedPlacementAndFocusIntents) {
    NotificationSurfaceIntentSource source;
    std::optional<NotificationSurfacePlacementIntent> placement;
    std::optional<NotificationFocusOrigin> returned;
    QObject::connect(&source, &NotificationSurfaceIntentSource::placementRequested,
                     [&placement](const auto &intent) { placement = intent; });
    QObject::connect(&source, &NotificationSurfaceIntentSource::focusReturnRequested,
                     [&returned](auto origin) { returned = origin; });

    ASSERT_TRUE(source.requestCenter(QStringLiteral("DP-1"), {42U}));
    ASSERT_TRUE(placement);
    EXPECT_EQ(placement->kind, NotificationSurfaceKind::center);
    EXPECT_EQ(placement->outputName, QStringLiteral("DP-1"));
    EXPECT_EQ(placement->focusOrigin, NotificationFocusOrigin{42U});

    ASSERT_TRUE(source.requestTransientStack(QStringLiteral("HDMI-1"), 9U));
    EXPECT_EQ(placement->kind, NotificationSurfaceKind::transientStack);
    EXPECT_EQ(placement->topologyGeneration, 9U);
    EXPECT_FALSE(placement->focusOrigin);

    ASSERT_TRUE(source.requestFocusReturn({42U}));
    EXPECT_EQ(returned, NotificationFocusOrigin{42U});
    EXPECT_FALSE(source.requestCenter(QString(129, QChar{'x'}), {42U}));
    EXPECT_FALSE(source.requestTransientStack({}, 9U));
    EXPECT_FALSE(source.requestFocusReturn({0U}));
}

} // namespace
} // namespace prismdrake::shell::notificationcenter
