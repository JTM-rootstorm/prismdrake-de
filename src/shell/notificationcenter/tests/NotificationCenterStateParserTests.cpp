#include "NotificationCenterStateParser.hpp"

#include "NotificationTypes.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace prismdrake::shell::notificationcenter {
namespace {

using Json = nlohmann::ordered_json;

[[nodiscard]] Json action(std::string id = "open", bool enabled = true) {
    return {{"action_id", std::move(id)}, {"label", "Open safely"}, {"enabled", enabled}};
}

[[nodiscard]] Json record(std::uint64_t recordId = 7U, std::uint64_t contentGeneration = 3U) {
    return {{"record_id", recordId},
            {"content_generation", contentGeneration},
            {"application_name", "Fixture"},
            {"desktop_file_id", "fixture.desktop"},
            {"summary", "Literal <b>summary</b>"},
            {"body", "Line one\nLine two"},
            {"urgency", "critical"},
            {"received_at_unix_ms", 1234U},
            {"dismissible", true},
            {"transient", true},
            {"actions", Json::array({action()})},
            {"notification_id", 9U}};
}

[[nodiscard]] Json document() {
    return {{"schema_version", 1U},
            {"generation", 4U},
            {"history_mode", "persistent"},
            {"effective_do_not_disturb", false},
            {"runtime_do_not_disturb_override", nullptr},
            {"records", Json::array({record()})}};
}

[[nodiscard]] foundation::Result<NotificationCenterSnapshotPtr> parse(const Json &value,
                                                                      std::uint64_t outer = 4U) {
    return parseNotificationCenterState(outer, value.dump());
}

TEST(NotificationCenterStateParserTest, ReconstructsCompleteCanonicalTypedState) {
    auto parsed = parse(document());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto &snapshot = *parsed.value();
    EXPECT_EQ(snapshot.generation, 4U);
    EXPECT_EQ(snapshot.historyMode, notifications::HistoryMode::persistent);
    EXPECT_FALSE(snapshot.effectiveDoNotDisturb);
    EXPECT_FALSE(snapshot.runtimeDoNotDisturbOverride);
    ASSERT_EQ(snapshot.records.size(), 1U);
    const auto &item = snapshot.records.front();
    EXPECT_EQ(item.recordId, 7U);
    EXPECT_EQ(item.contentGeneration, 3U);
    EXPECT_EQ(item.notificationId, 9U);
    EXPECT_EQ(item.summary, "Literal <b>summary</b>");
    EXPECT_EQ(item.body, "Line one\nLine two");
    EXPECT_EQ(item.urgency, notifications::ServiceUrgency::critical);
    ASSERT_EQ(item.actions.size(), 1U);
    EXPECT_TRUE(item.actions.front().enabled);
    EXPECT_EQ(snapshot.serializedJson, document().dump());
}

TEST(NotificationCenterStateParserTest, AcceptsClosedDeadHistoryWithoutLiveIdentity) {
    auto source = document();
    auto &item = source["records"][0U];
    item.erase("notification_id");
    item["transient"] = false;
    item["actions"][0U]["enabled"] = false;

    auto parsed = parse(source);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed.value()->records.front().notificationId);
    EXPECT_FALSE(parsed.value()->records.front().actions.front().enabled);
}

TEST(NotificationCenterStateParserTest, RejectsOuterVersionShapeAndCanonicalMismatches) {
    EXPECT_FALSE(parse(document(), 5U));

    auto unsupported = document();
    unsupported["schema_version"] = 2U;
    auto unsupportedResult = parse(unsupported);
    ASSERT_FALSE(unsupportedResult);
    EXPECT_EQ(unsupportedResult.error().code, foundation::ErrorCode::unsupported);

    auto extra = document();
    extra["private"] = "not accepted";
    EXPECT_FALSE(parse(extra));

    auto missing = document();
    missing.erase("history_mode");
    EXPECT_FALSE(parse(missing));

    const auto pretty = document().dump(2);
    EXPECT_FALSE(parseNotificationCenterState(4U, pretty));
}

TEST(NotificationCenterStateParserTest, RejectsDuplicateKeysAndBoundViolations) {
    constexpr auto duplicate =
        R"({"schema_version":1,"schema_version":1,"generation":4,"history_mode":"session","effective_do_not_disturb":false,"runtime_do_not_disturb_override":null,"records":[]})";
    EXPECT_FALSE(parseNotificationCenterState(4U, duplicate));

    auto tooMany = document();
    tooMany["records"] = Json::array();
    for (std::uint64_t index = 1U; index <= notifications::maximumServiceRecords + 1U; ++index) {
        auto item = record(index, 1U);
        item["notification_id"] = static_cast<std::uint32_t>(index);
        tooMany["records"].push_back(std::move(item));
    }
    EXPECT_FALSE(parse(tooMany));

    const std::string oversized(notifications::maximumNotificationStateBytes + 1U, 'x');
    auto oversizedResult = parseNotificationCenterState(4U, oversized);
    ASSERT_FALSE(oversizedResult);
    EXPECT_EQ(oversizedResult.error().code, foundation::ErrorCode::too_large);
}

TEST(NotificationCenterStateParserTest, RejectsDuplicateRecordLiveAndActionIdentities) {
    auto duplicateRecord = document();
    duplicateRecord["records"].push_back(record());
    EXPECT_FALSE(parse(duplicateRecord));

    auto duplicateLive = document();
    duplicateLive["records"].push_back(record(8U, 1U));
    EXPECT_FALSE(parse(duplicateLive));

    auto duplicateAction = document();
    duplicateAction["records"][0U]["actions"].push_back(action());
    EXPECT_FALSE(parse(duplicateAction));
}

TEST(NotificationCenterStateParserTest, RejectsImpossibleLivePresentationAndUnsafeText) {
    auto deadTransient = document();
    deadTransient["records"][0U].erase("notification_id");
    EXPECT_FALSE(parse(deadTransient));

    auto deadAction = document();
    deadAction["records"][0U].erase("notification_id");
    deadAction["records"][0U]["transient"] = false;
    EXPECT_FALSE(parse(deadAction));

    auto traversal = document();
    traversal["records"][0U]["desktop_file_id"] = "nested/fixture.desktop";
    EXPECT_FALSE(parse(traversal));

    auto control = document();
    control["records"][0U]["summary"] = std::string{"hidden\0tail", 11U};
    EXPECT_FALSE(parse(control));
}

} // namespace
} // namespace prismdrake::shell::notificationcenter
