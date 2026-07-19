#include "NotificationCenterStateParser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::shell::notificationcenter {
namespace {

using Json = nlohmann::ordered_json;
using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::size_t maximumNesting = 16U;
constexpr std::size_t maximumNodes = 16'384U;
constexpr std::size_t maximumContainerItems = notifications::maximumServiceRecords;
constexpr std::size_t maximumKeyCodePoints = 64U;

[[nodiscard]] Error invalidState(ErrorCode code = ErrorCode::validation_error) {
    return {code, "The complete notification-center state is invalid.",
            "Discard the publication and refetch after the next owner or state change."};
}

[[nodiscard]] std::size_t utf8CodePointCount(std::string_view value) noexcept {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }));
}

class StateBoundsSax final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t &) override { return false; }
    bool string(string_t &value) override {
        return utf8CodePointCount(value) <= notifications::maximumServiceBodyCodepoints && scalar();
    }
    bool binary(binary_t &) override { return false; }

    bool start_object(std::size_t elements) override { return startContainer(true, elements); }
    bool key(string_t &value) override {
        if (stack_.empty() || !stack_.back().object ||
            utf8CodePointCount(value) > maximumKeyCodePoints) {
            return false;
        }
        auto &frame = stack_.back();
        ++frame.items;
        return frame.items <= maximumContainerItems && frame.keys.insert(value).second;
    }
    bool end_object() override { return endContainer(true); }

    bool start_array(std::size_t elements) override { return startContainer(false, elements); }
    bool end_array() override { return endContainer(false); }

    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

  private:
    struct Frame final {
        bool object;
        std::size_t items;
        std::set<std::string, std::less<>> keys;
    };

    bool noteValue() {
        ++nodes_;
        if (nodes_ > maximumNodes || stack_.size() > maximumNesting) {
            return false;
        }
        if (!stack_.empty() && !stack_.back().object) {
            ++stack_.back().items;
            if (stack_.back().items > maximumContainerItems) {
                return false;
            }
        }
        return true;
    }

    bool scalar() { return noteValue(); }

    bool startContainer(bool object, std::size_t elements) {
        if (!noteValue() || stack_.size() >= maximumNesting ||
            (elements != std::numeric_limits<std::size_t>::max() &&
             elements > maximumContainerItems)) {
            return false;
        }
        stack_.push_back({object, 0U, {}});
        return true;
    }

    bool endContainer(bool object) {
        if (stack_.empty() || stack_.back().object != object) {
            return false;
        }
        stack_.pop_back();
        return true;
    }

    std::size_t nodes_{0U};
    std::vector<Frame> stack_;
};

template <std::size_t Size>
void requireExactKeys(const Json &object, const std::array<std::string_view, Size> &required,
                      std::optional<std::string_view> optional = std::nullopt) {
    if (!object.is_object() || object.size() < required.size() ||
        object.size() > required.size() + (optional ? 1U : 0U)) {
        throw std::domain_error("closed object expected");
    }
    for (const auto key : required) {
        if (!object.contains(key)) {
            throw std::domain_error("required member missing");
        }
    }
    for (const auto &[key, value] : object.items()) {
        (void)value;
        const bool known =
            std::ranges::find(required, key) != required.end() || (optional && key == *optional);
        if (!known) {
            throw std::domain_error("unknown member");
        }
    }
}

[[nodiscard]] std::uint64_t unsignedInteger(const Json &object, std::string_view key,
                                            std::uint64_t minimum, std::uint64_t maximum) {
    const auto &value = object.at(key);
    std::uint64_t result = 0U;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            throw std::domain_error("unsigned integer expected");
        }
        result = static_cast<std::uint64_t>(signedValue);
    } else {
        throw std::domain_error("integer expected");
    }
    if (result < minimum || result > maximum) {
        throw std::domain_error("bounded integer expected");
    }
    return result;
}

[[nodiscard]] bool booleanValue(const Json &object, std::string_view key) {
    const auto &value = object.at(key);
    if (!value.is_boolean()) {
        throw std::domain_error("boolean expected");
    }
    return value.get<bool>();
}

[[nodiscard]] std::string stringValue(const Json &object, std::string_view key,
                                      std::size_t maximumCodePoints, bool allowEmpty) {
    const auto &value = object.at(key);
    if (!value.is_string()) {
        throw std::domain_error("string expected");
    }
    auto result = value.get<std::string>();
    if ((!allowEmpty && result.empty()) || utf8CodePointCount(result) > maximumCodePoints) {
        throw std::domain_error("bounded string expected");
    }
    return result;
}

[[nodiscard]] std::optional<std::string> nullableString(const Json &object, std::string_view key,
                                                        std::size_t maximumCodePoints) {
    const auto &value = object.at(key);
    if (value.is_null()) {
        return std::nullopt;
    }
    return stringValue(object, key, maximumCodePoints, true);
}

[[nodiscard]] notifications::ServiceUrgency urgencyValue(const Json &object) {
    const auto urgency = stringValue(object, "urgency", 8U, false);
    if (urgency == "low") {
        return notifications::ServiceUrgency::low;
    }
    if (urgency == "normal") {
        return notifications::ServiceUrgency::normal;
    }
    if (urgency == "critical") {
        return notifications::ServiceUrgency::critical;
    }
    throw std::domain_error("closed urgency expected");
}

[[nodiscard]] notifications::HistoryMode historyModeValue(const Json &object) {
    const auto mode = stringValue(object, "history_mode", 10U, false);
    if (mode == "disabled") {
        return notifications::HistoryMode::disabled;
    }
    if (mode == "session") {
        return notifications::HistoryMode::session;
    }
    if (mode == "persistent") {
        return notifications::HistoryMode::persistent;
    }
    throw std::domain_error("closed history mode expected");
}

[[nodiscard]] std::optional<bool> runtimeOverrideValue(const Json &object) {
    const auto &value = object.at("runtime_do_not_disturb_override");
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_boolean()) {
        throw std::domain_error("nullable boolean expected");
    }
    return value.get<bool>();
}

[[nodiscard]] std::vector<notifications::ServiceAction> actionsValue(const Json &record) {
    constexpr std::array actionKeys{std::string_view{"action_id"}, std::string_view{"label"},
                                    std::string_view{"enabled"}};
    const auto &source = record.at("actions");
    if (!source.is_array() || source.size() > notifications::maximumServiceActions) {
        throw std::domain_error("bounded action list expected");
    }
    std::vector<notifications::ServiceAction> actions;
    std::set<std::string, std::less<>> identities;
    actions.reserve(source.size());
    for (const auto &item : source) {
        requireExactKeys(item, actionKeys);
        notifications::ServiceAction action{
            stringValue(item, "action_id", notifications::maximumServiceActionIdCodepoints, false),
            stringValue(item, "label", notifications::maximumServiceActionLabelCodepoints, false),
            booleanValue(item, "enabled")};
        if (!notifications::isValidServiceActionId(action.id) ||
            !identities.insert(action.id).second) {
            throw std::domain_error("unique action identity expected");
        }
        actions.push_back(std::move(action));
    }
    return actions;
}

[[nodiscard]] NotificationCenterRecord recordValue(const Json &source) {
    constexpr std::array recordKeys{
        std::string_view{"record_id"},        std::string_view{"content_generation"},
        std::string_view{"application_name"}, std::string_view{"desktop_file_id"},
        std::string_view{"summary"},          std::string_view{"body"},
        std::string_view{"urgency"},          std::string_view{"received_at_unix_ms"},
        std::string_view{"dismissible"},      std::string_view{"transient"},
        std::string_view{"actions"}};
    requireExactKeys(source, recordKeys, std::string_view{"notification_id"});

    NotificationCenterRecord record;
    record.recordId = unsignedInteger(source, "record_id", 1U, UINT64_MAX);
    record.contentGeneration = unsignedInteger(source, "content_generation", 1U, UINT64_MAX);
    if (source.contains("notification_id")) {
        record.notificationId =
            static_cast<std::uint32_t>(unsignedInteger(source, "notification_id", 1U, UINT32_MAX));
    }
    record.applicationName = nullableString(source, "application_name",
                                            notifications::maximumServiceApplicationNameCodepoints);
    record.desktopFileId = nullableString(source, "desktop_file_id",
                                          notifications::maximumServiceDesktopFileIdCodepoints);
    if (record.desktopFileId && !notifications::isValidDesktopFileId(*record.desktopFileId)) {
        throw std::domain_error("desktop file identity expected");
    }
    record.summary =
        stringValue(source, "summary", notifications::maximumServiceSummaryCodepoints, true);
    record.body = stringValue(source, "body", notifications::maximumServiceBodyCodepoints, true);
    record.urgency = urgencyValue(source);
    record.receivedAtUnixMilliseconds =
        unsignedInteger(source, "received_at_unix_ms", 0U, UINT64_MAX);
    record.dismissible = booleanValue(source, "dismissible");
    record.transientPresentation = booleanValue(source, "transient");
    record.actions = actionsValue(source);

    notifications::ServiceNotificationInput validation;
    validation.applicationName = record.applicationName;
    validation.desktopFileId = record.desktopFileId;
    validation.summary = record.summary;
    validation.body = record.body;
    validation.actions = record.actions;
    validation.urgency = record.urgency;
    validation.timeout = {notifications::ServiceTimeoutKind::never, {}};
    validation.dismissible = record.dismissible;
    validation.sender = ":1.1";
    validation.receivedAtUnixMilliseconds = record.receivedAtUnixMilliseconds;
    if (!notifications::validateServiceNotificationInput(validation)) {
        throw std::domain_error("valid literal notification record expected");
    }
    if (record.transientPresentation && !record.notificationId) {
        throw std::domain_error("transient records must be live");
    }
    if (std::ranges::any_of(record.actions, [](const auto &action) { return action.enabled; }) &&
        !record.notificationId) {
        throw std::domain_error("enabled actions must be live");
    }
    return record;
}

[[nodiscard]] Json canonicalJson(const NotificationCenterSnapshot &snapshot) {
    Json root = Json::object();
    root["schema_version"] = notificationCenterStateSchemaVersion;
    root["generation"] = snapshot.generation;
    switch (snapshot.historyMode) {
    case notifications::HistoryMode::disabled:
        root["history_mode"] = "disabled";
        break;
    case notifications::HistoryMode::session:
        root["history_mode"] = "session";
        break;
    case notifications::HistoryMode::persistent:
        root["history_mode"] = "persistent";
        break;
    }
    root["effective_do_not_disturb"] = snapshot.effectiveDoNotDisturb;
    root["runtime_do_not_disturb_override"] = snapshot.runtimeDoNotDisturbOverride
                                                  ? Json(*snapshot.runtimeDoNotDisturbOverride)
                                                  : Json(nullptr);
    root["records"] = Json::array();
    for (const auto &record : snapshot.records) {
        Json actions = Json::array();
        for (const auto &action : record.actions) {
            actions.push_back(
                {{"action_id", action.id}, {"label", action.label}, {"enabled", action.enabled}});
        }
        Json item = {
            {"record_id", record.recordId},
            {"content_generation", record.contentGeneration},
            {"application_name",
             record.applicationName ? Json(*record.applicationName) : Json(nullptr)},
            {"desktop_file_id", record.desktopFileId ? Json(*record.desktopFileId) : Json(nullptr)},
            {"summary", record.summary},
            {"body", record.body},
            {"urgency", record.urgency == notifications::ServiceUrgency::low        ? "low"
                        : record.urgency == notifications::ServiceUrgency::critical ? "critical"
                                                                                    : "normal"},
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
    return root;
}

} // namespace

Result<NotificationCenterSnapshotPtr> parseNotificationCenterState(std::uint64_t outerGeneration,
                                                                   std::string_view json) {
    if (outerGeneration == 0U || json.empty()) {
        return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
    }
    if (json.size() > notifications::maximumNotificationStateBytes) {
        return Result<NotificationCenterSnapshotPtr>::failure(invalidState(ErrorCode::too_large));
    }

    try {
        StateBoundsSax boundary;
        if (!Json::sax_parse(json.begin(), json.end(), &boundary)) {
            return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
        }
        auto document = Json::parse(json.begin(), json.end(), nullptr, false);
        constexpr std::array rootKeys{std::string_view{"schema_version"},
                                      std::string_view{"generation"},
                                      std::string_view{"history_mode"},
                                      std::string_view{"effective_do_not_disturb"},
                                      std::string_view{"runtime_do_not_disturb_override"},
                                      std::string_view{"records"}};
        requireExactKeys(document, rootKeys);
        if (unsignedInteger(document, "schema_version", 0U, UINT32_MAX) !=
            notificationCenterStateSchemaVersion) {
            return Result<NotificationCenterSnapshotPtr>::failure(
                invalidState(ErrorCode::unsupported));
        }

        auto snapshot = std::make_shared<NotificationCenterSnapshot>();
        snapshot->generation = unsignedInteger(document, "generation", 1U, UINT64_MAX);
        if (snapshot->generation != outerGeneration) {
            return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
        }
        snapshot->historyMode = historyModeValue(document);
        snapshot->effectiveDoNotDisturb = booleanValue(document, "effective_do_not_disturb");
        snapshot->runtimeDoNotDisturbOverride = runtimeOverrideValue(document);

        const auto &records = document.at("records");
        if (!records.is_array() || records.size() > notifications::maximumServiceRecords) {
            return Result<NotificationCenterSnapshotPtr>::failure(
                invalidState(ErrorCode::too_large));
        }
        snapshot->records.reserve(records.size());
        std::set<std::uint64_t> recordIds;
        std::set<std::uint32_t> notificationIds;
        for (const auto &source : records) {
            auto record = recordValue(source);
            if (!recordIds.insert(record.recordId).second ||
                (record.notificationId && !notificationIds.insert(*record.notificationId).second)) {
                return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
            }
            snapshot->records.push_back(std::move(record));
        }

        if (canonicalJson(*snapshot).dump() != json) {
            return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
        }
        snapshot->serializedJson.assign(json);
        return Result<NotificationCenterSnapshotPtr>::success(std::move(snapshot));
    } catch (const std::exception &) {
        return Result<NotificationCenterSnapshotPtr>::failure(invalidState());
    }
}

} // namespace prismdrake::shell::notificationcenter
