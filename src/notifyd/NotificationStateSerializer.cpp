#include "NotificationStateSerializer.hpp"

#include <string_view>

#include <nlohmann/json.hpp>

namespace prismdrake::notifyd {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using OrderedJson = nlohmann::ordered_json;

[[nodiscard]] std::string_view urgencyId(notifications::ServiceUrgency urgency) noexcept {
    switch (urgency) {
    case notifications::ServiceUrgency::low:
        return "low";
    case notifications::ServiceUrgency::normal:
        return "normal";
    case notifications::ServiceUrgency::critical:
        return "critical";
    }
    return "normal";
}

[[nodiscard]] std::string_view historyModeId(notifications::HistoryMode mode) noexcept {
    switch (mode) {
    case notifications::HistoryMode::disabled:
        return "disabled";
    case notifications::HistoryMode::session:
        return "session";
    case notifications::HistoryMode::persistent:
        return "persistent";
    }
    return "disabled";
}

} // namespace

Result<std::string> serializeNotificationState(const NotificationAuthority &authority) {
    OrderedJson root = OrderedJson::object();
    root["schema_version"] = 1U;
    root["generation"] = authority.generation();
    root["history_mode"] = historyModeId(authority.historyMode());
    root["effective_do_not_disturb"] = authority.effectiveDoNotDisturb();
    const auto runtimeOverride = authority.runtimeDoNotDisturbOverride();
    root["runtime_do_not_disturb_override"] =
        runtimeOverride ? OrderedJson(*runtimeOverride) : OrderedJson(nullptr);
    root["records"] = OrderedJson::array();

    if (authority.records().size() > notifications::maximumServiceRecords) {
        return Result<std::string>::failure(
            {ErrorCode::too_large, "The notification state exceeds its record bound.",
             "Discard the invalid publication and retain the prior complete state."});
    }
    for (const auto &record : authority.records()) {
        OrderedJson actions = OrderedJson::array();
        for (const auto &action : record.actions) {
            actions.push_back(
                {{"action_id", action.id}, {"label", action.label}, {"enabled", action.enabled}});
        }
        OrderedJson item = {
            {"record_id", record.recordId},
            {"content_generation", record.contentGeneration},
            {"application_name",
             record.applicationName ? OrderedJson(*record.applicationName) : OrderedJson(nullptr)},
            {"desktop_file_id",
             record.desktopFileId ? OrderedJson(*record.desktopFileId) : OrderedJson(nullptr)},
            {"summary", record.summary},
            {"body", record.body},
            {"urgency", urgencyId(record.urgency)},
            {"received_at_unix_ms", record.receivedAtUnixMilliseconds},
            {"dismissible", record.dismissible},
            {"transient", record.transientPresentation},
            {"actions", std::move(actions)},
        };
        if (record.notificationId) {
            item["notification_id"] = *record.notificationId;
        }
        root["records"].push_back(std::move(item));
    }
    auto encoded = root.dump();
    if (encoded.size() > notifications::maximumNotificationStateBytes) {
        return Result<std::string>::failure(
            {ErrorCode::too_large, "The notification state exceeds its byte bound.",
             "Discard the invalid publication and retain the prior complete state."});
    }
    return Result<std::string>::success(std::move(encoded));
}

} // namespace prismdrake::notifyd
