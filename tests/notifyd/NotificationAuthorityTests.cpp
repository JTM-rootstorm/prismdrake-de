#include "NotificationAuthority.hpp"
#include "NotificationHistory.hpp"
#include "NotificationStateSerializer.hpp"

#include "MonotonicClock.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::notifyd {
namespace {

using namespace std::chrono_literals;
using foundation::Error;
using foundation::ErrorCode;
using foundation::MonotonicClock;
using foundation::TestMonotonicClock;
using notifications::HistoryMode;
using notifications::ServiceAction;
using notifications::ServiceNotificationInput;
using notifications::ServiceTimeoutKind;
using notifications::ServiceUrgency;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::array<char, 48> name{};
        constexpr std::string_view pattern = "/tmp/prismdrake-notifyd.XXXXXX";
        std::copy(pattern.begin(), pattern.end(), name.begin());
        char *created = ::mkdtemp(name.data());
        if (created == nullptr) {
            throw std::runtime_error{"temporary directory creation failed"};
        }
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] ServiceNotificationInput notification(std::string summary = "Summary",
                                                    std::string sender = ":1.42") {
    ServiceNotificationInput input;
    input.applicationName = "Fixture App";
    input.desktopFileId = "org.prismdrake.Fixture.desktop";
    input.summary = std::move(summary);
    input.body = "Literal <b>body</b>";
    input.sender = std::move(sender);
    input.receivedAtUnixMilliseconds = 1234U;
    input.actions = {{"default", "Open", true}};
    return input;
}

[[nodiscard]] NotificationAuthority authority(const std::shared_ptr<TestMonotonicClock> &clock,
                                              AuthorityOptions options = AuthorityOptions{}) {
    auto created = NotificationAuthority::create(clock, std::move(options));
    if (!created) {
        throw std::runtime_error{created.error().message};
    }
    return std::move(created).value();
}

void expectRedacted(const Error &error, std::string_view privateValue) {
    EXPECT_EQ(error.message.find(privateValue), std::string::npos);
    EXPECT_EQ(error.recovery.find(privateValue), std::string::npos);
}

TEST(NotificationInputTest, AcceptsEmptySummaryAndOpaqueUnicodeActionKey) {
    auto input = notification("");
    input.actions = {{"開く\n", "Open", true}};
    EXPECT_TRUE(notifications::validateServiceNotificationInput(input));
}

TEST(NotificationInputTest, RejectsMalformedUtf8DuplicateActionsAndOversizedTextWithoutEcho) {
    auto invalidUtf8 = notification(std::string{"private\xC0\xAF", 9U});
    auto invalidResult = notifications::validateServiceNotificationInput(invalidUtf8);
    ASSERT_FALSE(invalidResult);
    EXPECT_EQ(invalidResult.error().code, ErrorCode::validation_error);
    expectRedacted(invalidResult.error(), "private");

    auto duplicates = notification();
    duplicates.actions.push_back(duplicates.actions.front());
    EXPECT_FALSE(notifications::validateServiceNotificationInput(duplicates));

    auto oversized = notification(std::string(notifications::maximumServiceSummaryBytes + 1U, 'x'));
    auto oversizedResult = notifications::validateServiceNotificationInput(oversized);
    ASSERT_FALSE(oversizedResult);
    EXPECT_EQ(oversizedResult.error().code, ErrorCode::too_large);
    expectRedacted(oversizedResult.error(), "xxx");
}

TEST(NotificationAuthorityTest, ReplacementRetainsIdsAdvancesContentAndTransfersSenderRouting) {
    auto clock = std::make_shared<TestMonotonicClock>(MonotonicClock::TimePoint{10s});
    auto owner = authority(clock);
    const auto first = owner.notify(notification("First", ":1.10"));
    ASSERT_TRUE(first);

    auto replacement = notification("Second", ":1.20");
    replacement.replacesId = first.value().notificationId;
    replacement.actions = {{"respond", "Respond", true}};
    const auto replaced = owner.notify(std::move(replacement));

    ASSERT_TRUE(replaced);
    EXPECT_TRUE(replaced.value().replaced);
    EXPECT_EQ(replaced.value().notificationId, first.value().notificationId);
    EXPECT_EQ(replaced.value().recordId, first.value().recordId);
    EXPECT_EQ(replaced.value().contentGeneration, first.value().contentGeneration + 1U);
    ASSERT_EQ(owner.records().size(), 1U);
    EXPECT_EQ(owner.records().front().summary, "Second");
    EXPECT_EQ(owner.records().front().sender, ":1.20");

    const auto action = owner.invokeAction(replaced.value().recordId,
                                           replaced.value().contentGeneration, "respond");
    ASSERT_TRUE(action);
    EXPECT_EQ(action.value().notificationId, first.value().notificationId);
    EXPECT_EQ(action.value().destination, ":1.20");
}

TEST(NotificationAuthorityTest, UnknownReplacementCreatesNewRecordWithRequestedLiveId) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    auto input = notification();
    input.replacesId = 900U;
    const auto created = owner.notify(std::move(input));
    ASSERT_TRUE(created);
    EXPECT_FALSE(created.value().replaced);
    EXPECT_EQ(created.value().notificationId, 900U);
    EXPECT_NE(created.value().recordId, 0U);
    ASSERT_EQ(owner.records().size(), 1U);
    EXPECT_EQ(owner.records().front().notificationId, 900U);
}

TEST(NotificationAuthorityTest, AllocationSkipsIdsClaimedByUnknownReplacementRequests) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    auto claimed = notification("Claimed");
    claimed.replacesId = 1U;
    const auto first = owner.notify(std::move(claimed));
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().notificationId, 1U);

    const auto allocated = owner.notify(notification("Allocated"));
    ASSERT_TRUE(allocated);
    EXPECT_EQ(allocated.value().notificationId, 2U);
    ASSERT_EQ(owner.records().size(), 2U);
    EXPECT_NE(owner.records()[0].notificationId, owner.records()[1].notificationId);
}

TEST(NotificationAuthorityTest, UnknownReplacementAcceptsMaximumLiveIdWithoutExhaustion) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    auto maximum = notification("Maximum");
    maximum.replacesId = std::numeric_limits<std::uint32_t>::max();
    const auto claimed = owner.notify(std::move(maximum));
    ASSERT_TRUE(claimed);
    EXPECT_EQ(claimed.value().notificationId, std::numeric_limits<std::uint32_t>::max());

    const auto allocated = owner.notify(notification("Allocated"));
    ASSERT_TRUE(allocated);
    EXPECT_EQ(allocated.value().notificationId, 1U);
}

TEST(NotificationAuthorityTest, DoNotDisturbSuppressesEligiblePresentationButCriticalBypasses) {
    auto clock = std::make_shared<TestMonotonicClock>();
    AuthorityOptions options;
    options.configuredDoNotDisturb = true;
    auto owner = authority(clock, options);

    ASSERT_TRUE(owner.notify(notification("Normal")));
    auto critical = notification("Critical");
    critical.urgency = ServiceUrgency::critical;
    ASSERT_TRUE(owner.notify(std::move(critical)));
    ASSERT_EQ(owner.records().size(), 2U);
    EXPECT_FALSE(owner.records()[0].transientPresentation);
    EXPECT_TRUE(owner.records()[1].transientPresentation);

    const auto priorGeneration = owner.generation();
    const auto same = owner.setDoNotDisturb(true);
    ASSERT_TRUE(same);
    EXPECT_EQ(same.value(), priorGeneration);
    const auto disabled = owner.setDoNotDisturb(false);
    ASSERT_TRUE(disabled);
    EXPECT_GT(disabled.value(), priorGeneration);
    EXPECT_TRUE(owner.records()[0].transientPresentation);
}

TEST(NotificationAuthorityTest, MonotonicTimeoutInvalidatesLiveIdBeforeClosePublication) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner =
        authority(clock, AuthorityOptions{HistoryMode::session, 100ms, false, true, std::nullopt});
    const auto created = owner.notify(notification());
    ASSERT_TRUE(created);
    ASSERT_TRUE(clock->advance(99ms));
    const auto before = owner.advanceTimeouts();
    ASSERT_TRUE(before);
    EXPECT_FALSE(before.value().published);

    ASSERT_TRUE(clock->advance(1ms));
    const auto expired = owner.advanceTimeouts();
    ASSERT_TRUE(expired);
    ASSERT_EQ(expired.value().closeEvents.size(), 1U);
    EXPECT_EQ(expired.value().closeEvents.front().notificationId, created.value().notificationId);
    EXPECT_EQ(expired.value().closeEvents.front().reason, CloseReason::expired);
    ASSERT_EQ(owner.records().size(), 1U);
    EXPECT_FALSE(owner.records().front().notificationId);
    EXPECT_FALSE(owner.close(created.value().notificationId, CloseReason::requested));
}

TEST(NotificationAuthorityTest, SenderLossDisablesActionsWithoutClosingLiveRecord) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    const auto created = owner.notify(notification("Sender", ":1.55"));
    ASSERT_TRUE(created);
    const auto lost = owner.senderDisappeared(":1.55");
    ASSERT_TRUE(lost);
    EXPECT_TRUE(lost.value().published);
    ASSERT_EQ(owner.records().size(), 1U);
    EXPECT_TRUE(owner.records().front().notificationId);
    EXPECT_FALSE(owner.records().front().sender);
    EXPECT_FALSE(owner.records().front().actions.front().enabled);
    EXPECT_FALSE(
        owner.invokeAction(created.value().recordId, created.value().contentGeneration, "default"));
}

TEST(NotificationAuthorityTest, StaleNativeCommandsHaveNoSideEffects) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    const auto created = owner.notify(notification());
    ASSERT_TRUE(created);
    const auto before = owner.generation();
    const auto stale =
        owner.dismiss(created.value().recordId, created.value().contentGeneration + 1U);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::cancelled);
    EXPECT_EQ(owner.generation(), before);
    EXPECT_EQ(owner.records().size(), 1U);
}

TEST(NotificationAuthorityTest, PersistentHistoryIsPrivateBoundedAndRestoresDisabledActions) {
    TemporaryDirectory temporary;
    const auto stateHome = temporary.path() / "state";
    ASSERT_TRUE(std::filesystem::create_directory(stateHome));
    ASSERT_EQ(::chmod(stateHome.c_str(), 0700), 0);
    const auto history = stateHome / "prismdrake" / "notifications" / "history-v1.json";
    auto clock = std::make_shared<TestMonotonicClock>();
    AuthorityOptions options{HistoryMode::persistent, 5s, false, true, history};
    auto owner = authority(clock, options);

    auto durable = notification("Durable", ":1.70");
    durable.body = "Private body";
    const auto created = owner.notify(std::move(durable));
    ASSERT_TRUE(created);
    ASSERT_TRUE(owner.close(created.value().notificationId, CloseReason::requested));
    ASSERT_TRUE(owner.flushHistory());

    struct stat metadata = {};
    ASSERT_EQ(::stat(history.c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777U, 0600U);
    std::ifstream stream(history);
    const std::string bytes{std::istreambuf_iterator<char>{stream}, {}};
    EXPECT_EQ(bytes.find("notification_id"), std::string::npos);
    EXPECT_EQ(bytes.find(":1.70"), std::string::npos);
    EXPECT_EQ(bytes.find("image"), std::string::npos);

    auto restarted = authority(clock, options);
    ASSERT_EQ(restarted.records().size(), 1U);
    EXPECT_EQ(restarted.records().front().summary, "Durable");
    EXPECT_FALSE(restarted.records().front().notificationId);
    EXPECT_FALSE(restarted.records().front().sender);
    ASSERT_EQ(restarted.records().front().actions.size(), 1U);
    EXPECT_FALSE(restarted.records().front().actions.front().enabled);
}

TEST(NotificationAuthorityTest, TransientHintNeverEntersPersistentHistory) {
    TemporaryDirectory temporary;
    const auto stateHome = temporary.path() / "state";
    ASSERT_TRUE(std::filesystem::create_directory(stateHome));
    const auto history = stateHome / "prismdrake" / "notifications" / "history-v1.json";
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock, {HistoryMode::persistent, 5s, false, true, history});
    auto transient = notification("Transient");
    transient.transientHint = true;
    const auto created = owner.notify(std::move(transient));
    ASSERT_TRUE(created);
    ASSERT_TRUE(owner.close(created.value().notificationId, CloseReason::requested));
    ASSERT_TRUE(owner.flushHistory());
    const auto loaded = loadNotificationHistory(history);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded.value().empty());
}

TEST(NotificationHistoryTest, RejectsUnknownDuplicateMalformedAndOversizedDocuments) {
    EXPECT_FALSE(parseNotificationHistory("{}"));
    EXPECT_FALSE(parseNotificationHistory(R"({"schema_version":1,"records":[],"unknown":true})"));
    const std::string duplicate =
        R"({"schema_version":1,"records":[{"record_id":1,"content_generation":1,"application_name":null,"desktop_file_id":null,"summary":"","body":"","urgency":"normal","received_at_unix_ms":0,"dismissible":true,"actions":[]},{"record_id":1,"content_generation":1,"application_name":null,"desktop_file_id":null,"summary":"","body":"","urgency":"normal","received_at_unix_ms":0,"dismissible":true,"actions":[]}]})";
    EXPECT_FALSE(parseNotificationHistory(duplicate));
    EXPECT_FALSE(parseNotificationHistory(
        std::string(notifications::maximumNotificationHistoryBytes + 1U, 'x')));
    EXPECT_FALSE(
        parseNotificationHistory(R"({"schema_version":1,"schema_version":1,"records":[]})"));
    EXPECT_FALSE(
        parseNotificationHistory(R"({"schema_version":1,"records":[[[[[[[[[[]]]]]]]]]]})"));
    std::string excessiveNodes = R"({"schema_version":1,"records":[)";
    for (std::size_t index = 0U; index < 17000U; ++index) {
        excessiveNodes += "null,";
    }
    excessiveNodes += "null]}";
    EXPECT_FALSE(parseNotificationHistory(excessiveNodes));
}

TEST(NotificationHistoryTest, RejectsSymlinkWorldReadableAndNonXdgHistoryFiles) {
    TemporaryDirectory temporary;
    const auto stateHome = temporary.path() / "state";
    const auto prismdrake = stateHome / "prismdrake";
    const auto notificationsDirectory = prismdrake / "notifications";
    ASSERT_TRUE(std::filesystem::create_directories(notificationsDirectory));
    ASSERT_EQ(::chmod(prismdrake.c_str(), 0700), 0);
    ASSERT_EQ(::chmod(notificationsDirectory.c_str(), 0700), 0);
    const auto history = notificationsDirectory / "history-v1.json";
    {
        std::ofstream stream(history);
        stream << R"({"schema_version":1,"records":[]})";
    }
    ASSERT_EQ(::chmod(history.c_str(), 0644), 0);
    EXPECT_FALSE(loadNotificationHistory(history));

    ASSERT_TRUE(std::filesystem::remove(history));
    const auto target = temporary.path() / "target.json";
    {
        std::ofstream stream(target);
        stream << R"({"schema_version":1,"records":[]})";
    }
    ASSERT_EQ(::chmod(target.c_str(), 0600), 0);
    std::error_code symlinkError;
    std::filesystem::create_symlink(target, history, symlinkError);
    ASSERT_FALSE(symlinkError);
    EXPECT_FALSE(loadNotificationHistory(history));
    EXPECT_FALSE(loadNotificationHistory(target));
}

TEST(NotificationStateTest, SerializesExactCompleteSchemaShape) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    const auto created = owner.notify(notification());
    ASSERT_TRUE(created);
    const auto state = serializeNotificationState(owner);
    ASSERT_TRUE(state);
    const auto json = nlohmann::json::parse(state.value());
    EXPECT_EQ(json.at("schema_version"), 1U);
    EXPECT_EQ(json.at("generation"), owner.generation());
    EXPECT_EQ(json.at("history_mode"), "session");
    EXPECT_FALSE(json.at("effective_do_not_disturb"));
    EXPECT_TRUE(json.at("runtime_do_not_disturb_override").is_null());
    ASSERT_EQ(json.at("records").size(), 1U);
    const auto &record = json.at("records").front();
    EXPECT_EQ(record.at("record_id"), created.value().recordId);
    EXPECT_EQ(record.at("notification_id"), created.value().notificationId);
    EXPECT_TRUE(record.at("transient"));
    EXPECT_TRUE(record.at("actions").front().at("enabled"));
}

TEST(NotificationAuthorityTest, RejectsMoreThanBoundedConcurrentLiveRecords) {
    auto clock = std::make_shared<TestMonotonicClock>();
    auto owner = authority(clock);
    for (std::size_t index = 0U; index < notifications::maximumServiceRecords; ++index) {
        auto input = notification(std::to_string(index));
        input.timeout = {ServiceTimeoutKind::never, 0ms};
        ASSERT_TRUE(owner.notify(std::move(input)));
    }
    const auto overflow = owner.notify(notification("overflow"));
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::too_large);
    expectRedacted(overflow.error(), "overflow");
}

} // namespace
} // namespace prismdrake::notifyd
