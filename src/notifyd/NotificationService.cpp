#include "NotificationService.hpp"

#include "BuildInfo.hpp"
#include "NotificationStateSerializer.hpp"
#include "PeerCredentials.hpp"
#include "SdBusApi.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <poll.h>

namespace prismdrake::notifyd {
namespace {

namespace sdbus = ipc::sdbus;

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using notifications::HistoryMode;
using notifications::ServiceAction;
using notifications::ServiceNotificationInput;
using notifications::ServiceTimeoutKind;
using notifications::ServiceUrgency;

constexpr char standardServiceName[] = "org.freedesktop.Notifications";
constexpr char standardObjectPath[] = "/org/freedesktop/Notifications";
constexpr char standardInterface[] = "org.freedesktop.Notifications";
constexpr char centerServiceName[] = "org.prismdrake.NotificationCenter1";
constexpr char centerObjectPath[] = "/org/prismdrake/NotificationCenter1";
constexpr char centerInterface[] = "org.prismdrake.NotificationCenter1";
constexpr char settingsServiceName[] = "org.prismdrake.Settings1";
constexpr char settingsObjectPath[] = "/org/prismdrake/Settings1";
constexpr char settingsSnapshotInterface[] = "org.prismdrake.SettingsSnapshot1";
constexpr char settingsInterface[] = "org.prismdrake.Settings1";

constexpr char invalidArgumentsError[] = "org.freedesktop.DBus.Error.InvalidArgs";
constexpr char limitsExceededError[] = "org.freedesktop.DBus.Error.LimitsExceeded";
constexpr char invalidNotificationIdError[] = "org.freedesktop.Notifications.Error.InvalidId";

constexpr char unsupportedSchemaError[] =
    "org.prismdrake.NotificationCenter1.Error.UnsupportedSchemaVersion";
constexpr char staleRecordError[] = "org.prismdrake.NotificationCenter1.Error.StaleRecord";
constexpr char actionUnavailableError[] =
    "org.prismdrake.NotificationCenter1.Error.ActionUnavailable";
constexpr char invalidActionError[] = "org.prismdrake.NotificationCenter1.Error.InvalidAction";
constexpr char tooLargeError[] = "org.prismdrake.NotificationCenter1.Error.TooLarge";
constexpr char notAuthorizedError[] = "org.prismdrake.NotificationCenter1.Error.NotAuthorized";
constexpr char internalError[] = "org.prismdrake.NotificationCenter1.Error.Internal";

constexpr char invalidArgumentsMessage[] = "The notification request is invalid.";
constexpr char limitsExceededMessage[] = "The notification request exceeds a fixed bound.";
constexpr char unsupportedSchemaMessage[] = "The requested state schema is unsupported.";
constexpr char staleRecordMessage[] = "The notification record is stale.";
constexpr char actionUnavailableMessage[] = "The notification action is unavailable.";
constexpr char invalidActionMessage[] = "The notification action identifier is invalid.";
constexpr char tooLargeMessage[] = "The notification request exceeds a fixed bound.";
constexpr char notAuthorizedMessage[] = "The notification-center request is not authorized.";
constexpr char internalMessage[] = "The notification service could not complete the request.";

constexpr std::size_t maximumIgnoredIconBytes = 4096U;
constexpr std::size_t maximumHintEntries = 64U;
constexpr std::size_t maximumHintKeyBytes = 128U;
constexpr std::size_t maximumHintNodes = 1024U;
constexpr std::size_t maximumHintDepth = 8U;
constexpr std::size_t maximumHintStringBytes = 16U * 1024U;
constexpr std::size_t maximumHintByteArrayBytes = 1024U * 1024U;
constexpr std::size_t maximumSettingsSnapshotBytes = 1024U * 1024U;
constexpr std::size_t maximumSettingsDepth = 16U;
constexpr std::size_t maximumSettingsNodes = 16U * 1024U;

class SettingsSaxBoundary final : public nlohmann::json_sax<nlohmann::json> {
  public:
    bool null() override { return consume(); }
    bool boolean(bool) override { return consume(); }
    bool number_integer(number_integer_t) override { return consume(); }
    bool number_unsigned(number_unsigned_t) override { return consume(); }
    bool number_float(number_float_t, const string_t &) override { return consume(); }
    bool string(string_t &value) override {
        return value.size() <= maximumHintStringBytes && consume();
    }
    bool binary(binary_t &) override { return false; }
    bool start_object(std::size_t) override {
        if (!consume() || containers_.size() >= maximumSettingsDepth) {
            return false;
        }
        containers_.emplace_back(std::set<std::string>{});
        return true;
    }
    bool key(string_t &value) override {
        return value.size() <= maximumHintKeyBytes && !containers_.empty() &&
               containers_.back().has_value() && containers_.back()->insert(value).second &&
               consume();
    }
    bool end_object() override { return endContainer(true); }
    bool start_array(std::size_t) override {
        if (!consume() || containers_.size() >= maximumSettingsDepth) {
            return false;
        }
        containers_.emplace_back(std::nullopt);
        return true;
    }
    bool end_array() override { return endContainer(false); }
    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }

  private:
    [[nodiscard]] bool consume() noexcept {
        if (nodes_ >= maximumSettingsNodes) {
            return false;
        }
        ++nodes_;
        return true;
    }
    [[nodiscard]] bool endContainer(bool object) {
        if (containers_.empty() || containers_.back().has_value() != object) {
            return false;
        }
        containers_.pop_back();
        return true;
    }

    std::size_t nodes_{0U};
    std::vector<std::optional<std::set<std::string>>> containers_;
};

[[nodiscard]] Error serviceError(ErrorCode code, std::string message) {
    return {code, std::move(message), "Begin a fresh bounded notification owner epoch."};
}

[[nodiscard]] int setBusError(sd_bus_error *error, const char *name, const char *message) {
    return sd_bus_error_set_const(error, name, message);
}

[[nodiscard]] int appendStringArray(sd_bus_message *message,
                                    const std::vector<std::string> &values) {
    int result = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "s");
    if (result < 0) {
        return result;
    }
    for (const auto &value : values) {
        result = sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, value.c_str());
        if (result < 0) {
            return result;
        }
    }
    return sd_bus_message_close_container(message);
}

[[nodiscard]] bool boundedString(const char *value, std::size_t maximumBytes) noexcept {
    return value != nullptr && std::char_traits<char>::length(value) <= maximumBytes;
}

[[nodiscard]] int skipBoundedValue(sd_bus_message *message, std::size_t depth, std::size_t &nodes,
                                   std::size_t &byteBudget) {
    if (depth > maximumHintDepth || nodes >= maximumHintNodes) {
        return -E2BIG;
    }
    ++nodes;
    char type = 0;
    const char *contents = nullptr;
    const int peeked = sd_bus_message_peek_type(message, &type, &contents);
    if (peeked <= 0) {
        return peeked == 0 ? -EINVAL : peeked;
    }

    if (type == SD_BUS_TYPE_STRING || type == SD_BUS_TYPE_OBJECT_PATH ||
        type == SD_BUS_TYPE_SIGNATURE) {
        const char *value = nullptr;
        const int result = sd_bus_message_read_basic(message, type, &value);
        if (result <= 0 || !boundedString(value, maximumHintStringBytes)) {
            return -E2BIG;
        }
        const auto size = std::char_traits<char>::length(value);
        if (byteBudget > maximumHintByteArrayBytes - size) {
            return -E2BIG;
        }
        byteBudget += size;
        return 0;
    }
    if (type == SD_BUS_TYPE_ARRAY && contents != nullptr && std::string_view{contents} == "y") {
        const void *bytes = nullptr;
        std::size_t size = 0U;
        const int result = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes, &size);
        if (result < 0 || size > maximumHintByteArrayBytes ||
            byteBudget > maximumHintByteArrayBytes - size) {
            return -E2BIG;
        }
        byteBudget += size;
        return 0;
    }
    if (type == SD_BUS_TYPE_ARRAY || type == SD_BUS_TYPE_STRUCT || type == SD_BUS_TYPE_DICT_ENTRY ||
        type == SD_BUS_TYPE_VARIANT) {
        if (contents == nullptr || sd_bus_message_enter_container(message, type, contents) <= 0) {
            return -EINVAL;
        }
        std::size_t entries = 0U;
        while (sd_bus_message_peek_type(message, nullptr, nullptr) > 0) {
            if (++entries > maximumHintNodes) {
                return -E2BIG;
            }
            const int skipped = skipBoundedValue(message, depth + 1U, nodes, byteBudget);
            if (skipped < 0) {
                return skipped;
            }
        }
        return sd_bus_message_exit_container(message) < 0 ? -EINVAL : 0;
    }

    std::array<char, 2U> signature{type, '\0'};
    return sd_bus_message_skip(message, signature.data()) < 0 ? -EINVAL : 0;
}

[[nodiscard]] Result<void> parseActions(sd_bus_message *message,
                                        std::vector<ServiceAction> &actions) {
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") <= 0) {
        return Result<void>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    std::vector<std::string> values;
    values.reserve(notifications::maximumServiceActions * 2U);
    const char *value = nullptr;
    int read = 0;
    while ((read = sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &value)) > 0) {
        if (values.size() >= notifications::maximumServiceActions * 2U || value == nullptr ||
            std::char_traits<char>::length(value) > notifications::maximumServiceActionLabelBytes) {
            return Result<void>::failure(serviceError(ErrorCode::too_large, limitsExceededMessage));
        }
        values.emplace_back(value);
    }
    if (read < 0 || sd_bus_message_exit_container(message) < 0 || values.size() % 2U != 0U) {
        return Result<void>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    actions.reserve(values.size() / 2U);
    for (std::size_t index = 0U; index < values.size(); index += 2U) {
        actions.push_back({std::move(values[index]), std::move(values[index + 1U]), true});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> parseHints(sd_bus_message *message, ServiceNotificationInput &input) {
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
        return Result<void>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    std::size_t entries = 0U;
    std::size_t nodes = 0U;
    std::size_t byteBudget = 0U;
    std::set<std::string> hintKeys;
    int entered = 0;
    while ((entered = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        if (++entries > maximumHintEntries) {
            return Result<void>::failure(serviceError(ErrorCode::too_large, limitsExceededMessage));
        }
        const char *key = nullptr;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &key) <= 0 ||
            !boundedString(key, maximumHintKeyBytes) || !hintKeys.insert(key).second) {
            return Result<void>::failure(
                serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
        }
        char variantType = 0;
        const char *variantContents = nullptr;
        if (sd_bus_message_peek_type(message, &variantType, &variantContents) <= 0 ||
            variantType != SD_BUS_TYPE_VARIANT || variantContents == nullptr ||
            sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, variantContents) <= 0) {
            return Result<void>::failure(
                serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
        }

        const std::string_view hint{key};
        if (hint == "urgency") {
            std::uint8_t urgency = 0U;
            if (std::string_view{variantContents} != "y" ||
                sd_bus_message_read_basic(message, SD_BUS_TYPE_BYTE, &urgency) <= 0 ||
                urgency > 2U) {
                return Result<void>::failure(
                    serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
            }
            input.urgency = static_cast<ServiceUrgency>(urgency);
        } else if (hint == "transient") {
            int transient = 0;
            if (std::string_view{variantContents} != "b" ||
                sd_bus_message_read_basic(message, SD_BUS_TYPE_BOOLEAN, &transient) <= 0) {
                return Result<void>::failure(
                    serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
            }
            input.transientHint = transient != 0;
        } else if (hint == "desktop-entry") {
            const char *desktopEntry = nullptr;
            if (std::string_view{variantContents} != "s" ||
                sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &desktopEntry) <= 0 ||
                !boundedString(desktopEntry, notifications::maximumServiceDesktopFileIdBytes)) {
                return Result<void>::failure(
                    serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
            }
            std::string identifier{desktopEntry};
            if (!identifier.ends_with(".desktop")) {
                identifier += ".desktop";
            }
            input.desktopFileId = std::move(identifier);
        } else {
            const int skipped = skipBoundedValue(message, 1U, nodes, byteBudget);
            if (skipped < 0) {
                return Result<void>::failure(serviceError(
                    skipped == -E2BIG ? ErrorCode::too_large : ErrorCode::validation_error,
                    skipped == -E2BIG ? limitsExceededMessage : invalidArgumentsMessage));
            }
        }
        if (sd_bus_message_exit_container(message) < 0 ||
            sd_bus_message_exit_container(message) < 0) {
            return Result<void>::failure(
                serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
        }
    }
    if (entered < 0 || sd_bus_message_exit_container(message) < 0) {
        return Result<void>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<ServiceNotificationInput> parseNotify(sd_bus_message *message) {
    const char *applicationName = nullptr;
    const char *ignoredIcon = nullptr;
    const char *summary = nullptr;
    const char *body = nullptr;
    ServiceNotificationInput input;
    if (sd_bus_message_read(message, "susss", &applicationName, &input.replacesId, &ignoredIcon,
                            &summary, &body) < 0 ||
        applicationName == nullptr || ignoredIcon == nullptr || summary == nullptr ||
        body == nullptr) {
        return Result<ServiceNotificationInput>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    if (!boundedString(applicationName, notifications::maximumServiceApplicationNameBytes) ||
        !boundedString(ignoredIcon, maximumIgnoredIconBytes) ||
        !boundedString(summary, notifications::maximumServiceSummaryBytes) ||
        !boundedString(body, notifications::maximumServiceBodyBytes)) {
        return Result<ServiceNotificationInput>::failure(
            serviceError(ErrorCode::too_large, limitsExceededMessage));
    }
    if (*applicationName != '\0') {
        input.applicationName = applicationName;
    }
    input.summary = summary;
    input.body = body;
    auto actions = parseActions(message, input.actions);
    if (!actions) {
        return Result<ServiceNotificationInput>::failure(actions.error());
    }
    auto hints = parseHints(message, input);
    if (!hints) {
        return Result<ServiceNotificationInput>::failure(hints.error());
    }
    std::int32_t timeout = 0;
    if (sd_bus_message_read_basic(message, SD_BUS_TYPE_INT32, &timeout) <= 0 || timeout < -1) {
        return Result<ServiceNotificationInput>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    if (timeout == -1) {
        input.timeout = {ServiceTimeoutKind::configured_default, std::chrono::milliseconds{0}};
    } else if (timeout == 0) {
        input.timeout = {ServiceTimeoutKind::never, std::chrono::milliseconds{0}};
    } else {
        input.timeout = {ServiceTimeoutKind::explicit_timeout, std::chrono::milliseconds{timeout}};
    }
    const char *sender = sd_bus_message_get_sender(message);
    if (sender == nullptr) {
        return Result<ServiceNotificationInput>::failure(
            serviceError(ErrorCode::validation_error, invalidArgumentsMessage));
    }
    input.sender = sender;
    const auto unixNow = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    input.receivedAtUnixMilliseconds = unixNow < 0 ? 0U : static_cast<std::uint64_t>(unixNow);
    auto valid = notifications::validateServiceNotificationInput(input);
    if (!valid) {
        return Result<ServiceNotificationInput>::failure(valid.error());
    }
    return Result<ServiceNotificationInput>::success(std::move(input));
}

[[nodiscard]] std::optional<SettingsInput> parseSettingsSnapshot(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > maximumSettingsSnapshotBytes) {
        return std::nullopt;
    }
    SettingsSaxBoundary boundary;
    if (!nlohmann::json::sax_parse(bytes.begin(), bytes.end(), &boundary)) {
        return std::nullopt;
    }
    auto json = nlohmann::json::parse(bytes, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("schema_version", 0U) != 2U ||
        !json.contains("settings") || !json["settings"].is_object() ||
        !json["settings"].contains("notifications")) {
        return std::nullopt;
    }
    const auto &value = json["settings"]["notifications"];
    if (!value.is_object() || !value.contains("enabled") || !value["enabled"].is_boolean() ||
        !value.contains("history") || !value["history"].is_string() ||
        !value.contains("default_timeout_ms") ||
        !value["default_timeout_ms"].is_number_unsigned() || !value.contains("do_not_disturb") ||
        !value["do_not_disturb"].is_boolean()) {
        return std::nullopt;
    }
    HistoryMode history = HistoryMode::session;
    const auto historyId = value["history"].get<std::string>();
    if (historyId == "disabled") {
        history = HistoryMode::disabled;
    } else if (historyId == "persistent") {
        history = HistoryMode::persistent;
    } else if (historyId != "session") {
        return std::nullopt;
    }
    const auto timeout = value["default_timeout_ms"].get<std::uint64_t>();
    if (timeout > 120000U) {
        return std::nullopt;
    }
    return SettingsInput{history, std::chrono::milliseconds{timeout},
                         value["do_not_disturb"].get<bool>(), value["enabled"].get<bool>()};
}

class NotificationService final {
  public:
    NotificationService(sd_bus *bus, NotificationAuthority authority)
        : bus_(bus), authority_(std::move(authority)) {}

    [[nodiscard]] Result<void> attach() {
        int result = sd_bus_add_object_vtable(bus_, standard_slot_.put(), standardObjectPath,
                                              standardInterface, standardVtable(), this);
        if (result < 0) {
            return Result<void>::failure(
                serviceError(ErrorCode::io_error,
                             "The standard notification interface could not be registered."));
        }
        result = sd_bus_add_object_vtable(bus_, center_slot_.put(), centerObjectPath,
                                          centerInterface, centerVtable(), this);
        if (result < 0) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "The notification-center interface could not be registered."));
        }
        result = sd_bus_match_signal(bus_, owner_slot_.put(), "org.freedesktop.DBus",
                                     "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                     "NameOwnerChanged", handleNameOwnerChanged, this);
        if (result < 0) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "Notification sender lifecycle could not be monitored."));
        }
        result = sd_bus_match_signal(bus_, settings_slot_.put(), settingsServiceName,
                                     settingsObjectPath, settingsInterface,
                                     "SettingsGenerationChanged", handleSettingsChanged, this);
        if (result < 0) {
            return Result<void>::failure(serviceError(
                ErrorCode::io_error, "Notification settings changes could not be monitored."));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> refreshSettings() {
        settings_refresh_requested_ = false;
        sdbus::Message call;
        sdbus::Message reply;
        if (sd_bus_message_new_method_call(bus_, call.put(), settingsServiceName,
                                           settingsObjectPath, settingsSnapshotInterface,
                                           "GetCurrentSnapshot") < 0 ||
            sd_bus_message_append(call.get(), "u", 2U) < 0) {
            return Result<void>::success();
        }
        sd_bus_error error{};
        constexpr std::uint64_t timeoutMicroseconds = 1000000U;
        const int called = sd_bus_call(bus_, call.get(), timeoutMicroseconds, &error, reply.put());
        sd_bus_error_free(&error);
        if (called < 0) {
            return Result<void>::success();
        }
        std::uint64_t generation = 0U;
        const void *bytes = nullptr;
        std::size_t size = 0U;
        if (sd_bus_message_read(reply.get(), "t", &generation) < 0 || generation == 0U ||
            sd_bus_message_read_array(reply.get(), SD_BUS_TYPE_BYTE, &bytes, &size) < 0 ||
            size == 0U || size > maximumSettingsSnapshotBytes || bytes == nullptr ||
            generation <= settings_generation_) {
            return Result<void>::success();
        }
        auto settings = parseSettingsSnapshot(
            {static_cast<const char *>(bytes), static_cast<std::size_t>(size)});
        if (!settings) {
            return Result<void>::success();
        }
        const auto prior = authority_.generation();
        auto applied = authority_.applySettings(*settings);
        if (!applied) {
            return Result<void>::success();
        }
        settings_generation_ = generation;
        reportPersistenceDegradation();
        if (authority_.generation() != prior) {
            observePostCommitSignal(
                emitStateChanged({"records", "do_not_disturb", "history_mode"}));
        }
        return Result<void>::success();
    }

    [[nodiscard]] bool settingsRefreshRequested() const noexcept {
        return settings_refresh_requested_;
    }

    [[nodiscard]] bool transportDegraded() const noexcept {
        return post_commit_transport_.degraded();
    }

    [[nodiscard]] Result<void> dispatchTimeouts() {
        auto outcome = authority_.advanceTimeouts();
        if (!outcome) {
            return Result<void>::failure(outcome.error());
        }
        if (!outcome.value().published) {
            return Result<void>::success();
        }
        observePostCommitSignal(emitStateChanged({"records"}));
        for (const auto &event : outcome.value().closeEvents) {
            observePostCommitSignal(emitClose(event));
        }
        reportPersistenceDegradation();
        return Result<void>::success();
    }

    [[nodiscard]] int pollTimeoutMilliseconds() const noexcept {
        int timeout = -1;
        const auto expiry = authority_.nextExpiry();
        if (expiry) {
            const auto remaining = *expiry - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                timeout = 0;
            } else {
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              remaining + std::chrono::microseconds{999})
                                              .count();
                timeout = milliseconds > INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
            }
        }

        std::uint64_t busDeadline = UINT64_MAX;
        if (sd_bus_get_timeout(bus_, &busDeadline) >= 0 && busDeadline != UINT64_MAX) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                const auto nowMicroseconds = static_cast<std::uint64_t>(now.tv_sec) * 1000000U +
                                             static_cast<std::uint64_t>(now.tv_nsec / 1000);
                int busTimeout = 0;
                if (busDeadline > nowMicroseconds) {
                    const auto remainingMicroseconds = busDeadline - nowMicroseconds;
                    const auto milliseconds =
                        remainingMicroseconds / 1000U + (remainingMicroseconds % 1000U != 0U);
                    busTimeout = milliseconds > static_cast<std::uint64_t>(INT_MAX)
                                     ? INT_MAX
                                     : static_cast<int>(milliseconds);
                }
                timeout = timeout < 0 ? busTimeout : std::min(timeout, busTimeout);
            }
        }
        return timeout;
    }

    [[nodiscard]] Result<void> flushHistory() const { return authority_.flushHistory(); }

  private:
    [[nodiscard]] static const sd_bus_vtable *standardVtable() {
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCapabilities", "", "as", handleGetCapabilities,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", handleNotify, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("CloseNotification", "u", "", handleCloseNotification,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("GetServerInformation", "", "ssss", handleGetServerInformation,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
            SD_BUS_SIGNAL("ActionInvoked", "us", 0),
            SD_BUS_VTABLE_END,
        };
        return vtable;
    }

    [[nodiscard]] static const sd_bus_vtable *centerVtable() {
        static const sd_bus_vtable vtable[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCurrentState", "u", "tay", handleGetCurrentState,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SetDoNotDisturb", "b", "t", handleSetDoNotDisturb,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ClearHistory", "", "t", handleClearHistory, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Dismiss", "tt", "t", handleDismiss, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("InvokeAction", "tts", "t", handleInvokeAction,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_SIGNAL("StateChanged", "tas", 0),
            SD_BUS_VTABLE_END,
        };
        return vtable;
    }

    [[nodiscard]] static NotificationService &from(void *userData) {
        return *static_cast<NotificationService *>(userData);
    }

    [[nodiscard]] static int authorize(sd_bus_message *message, sd_bus_error *error) {
        return ipc::isSameUserPeer(message)
                   ? 0
                   : setBusError(error, notAuthorizedError, notAuthorizedMessage);
    }

    [[nodiscard]] static int mapStandardError(sd_bus_error *error, const Error &value) {
        return setBusError(
            error, value.code == ErrorCode::too_large ? limitsExceededError : invalidArgumentsError,
            value.code == ErrorCode::too_large ? limitsExceededMessage : invalidArgumentsMessage);
    }

    [[nodiscard]] static int handleGetCapabilities(sd_bus_message *message, void *,
                                                   sd_bus_error *) {
        sdbus::Message reply;
        int result = sd_bus_message_new_method_return(message, reply.put());
        if (result >= 0) {
            result = appendStringArray(reply.get(), {"actions", "body", "persistence"});
        }
        if (result >= 0) {
            result = sd_bus_send(sd_bus_message_get_bus(message), reply.get(), nullptr);
        }
        return result;
    }

    [[nodiscard]] static int handleGetServerInformation(sd_bus_message *message, void *,
                                                        sd_bus_error *) {
        const std::string version{foundation::productVersion()};
        return sd_bus_reply_method_return(message, "ssss", "Prismdrake", "Prismdrake",
                                          version.c_str(), "1.3");
    }

    [[nodiscard]] static int handleNotify(sd_bus_message *message, void *userData,
                                          sd_bus_error *error) {
        auto input = parseNotify(message);
        if (!input) {
            return mapStandardError(error, input.error());
        }
        auto &service = from(userData);
        auto notified = service.authority_.notify(std::move(input).value());
        if (!notified) {
            return mapStandardError(error, notified.error());
        }
        service.observePostCommitSignal(service.emitStateChanged({"records"}));
        return sd_bus_reply_method_return(message, "u", notified.value().notificationId);
    }

    [[nodiscard]] static int handleCloseNotification(sd_bus_message *message, void *userData,
                                                     sd_bus_error *error) {
        std::uint32_t notificationId = 0U;
        if (sd_bus_message_read(message, "u", &notificationId) < 0 || notificationId == 0U) {
            return setBusError(error, invalidArgumentsError, invalidArgumentsMessage);
        }
        auto &service = from(userData);
        auto closed = service.authority_.close(notificationId, CloseReason::requested);
        if (!closed) {
            return setBusError(error, invalidNotificationIdError, "");
        }
        service.observePostCommitSignal(service.emitStateChanged({"records"}));
        if (closed.value().event) {
            service.observePostCommitSignal(service.emitClose(*closed.value().event));
        }
        service.reportPersistenceDegradation();
        return sd_bus_reply_method_return(message, "");
    }

    [[nodiscard]] static int handleGetCurrentState(sd_bus_message *message, void *userData,
                                                   sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        std::uint32_t schemaVersion = 0U;
        if (sd_bus_message_read(message, "u", &schemaVersion) < 0) {
            return setBusError(error, internalError, internalMessage);
        }
        if (schemaVersion != 1U) {
            return setBusError(error, unsupportedSchemaError, unsupportedSchemaMessage);
        }
        auto &service = from(userData);
        auto serialized = serializeNotificationState(service.authority_);
        if (!serialized) {
            return setBusError(error,
                               serialized.error().code == ErrorCode::too_large ? tooLargeError
                                                                               : internalError,
                               serialized.error().code == ErrorCode::too_large ? tooLargeMessage
                                                                               : internalMessage);
        }
        sdbus::Message reply;
        int result = sd_bus_message_new_method_return(message, reply.put());
        if (result >= 0) {
            result = sd_bus_message_append(reply.get(), "t", service.authority_.generation());
        }
        if (result >= 0) {
            result =
                sd_bus_message_append_array(reply.get(), SD_BUS_TYPE_BYTE,
                                            serialized.value().data(), serialized.value().size());
        }
        if (result >= 0) {
            result = sd_bus_send(service.bus_, reply.get(), nullptr);
        }
        return result;
    }

    [[nodiscard]] static int handleSetDoNotDisturb(sd_bus_message *message, void *userData,
                                                   sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        int enabled = 0;
        if (sd_bus_message_read(message, "b", &enabled) < 0) {
            return setBusError(error, internalError, internalMessage);
        }
        auto &service = from(userData);
        const auto prior = service.authority_.generation();
        auto changed = service.authority_.setDoNotDisturb(enabled != 0);
        if (!changed) {
            return setBusError(error, internalError, internalMessage);
        }
        if (changed.value() != prior) {
            service.observePostCommitSignal(
                service.emitStateChanged({"records", "do_not_disturb"}));
        }
        return sd_bus_reply_method_return(message, "t", changed.value());
    }

    [[nodiscard]] static int handleClearHistory(sd_bus_message *message, void *userData,
                                                sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        auto &service = from(userData);
        const auto prior = service.authority_.generation();
        auto cleared = service.authority_.clearHistory();
        if (!cleared) {
            return setBusError(error, internalError, internalMessage);
        }
        if (cleared.value() != prior) {
            service.observePostCommitSignal(service.emitStateChanged({"records"}));
        }
        return sd_bus_reply_method_return(message, "t", cleared.value());
    }

    [[nodiscard]] static int handleDismiss(sd_bus_message *message, void *userData,
                                           sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        std::uint64_t recordId = 0U;
        std::uint64_t contentGeneration = 0U;
        if (sd_bus_message_read(message, "tt", &recordId, &contentGeneration) < 0) {
            return setBusError(error, internalError, internalMessage);
        }
        auto &service = from(userData);
        auto dismissed = service.authority_.dismiss(recordId, contentGeneration);
        if (!dismissed) {
            return setBusError(error,
                               dismissed.error().code == ErrorCode::cancelled ? staleRecordError
                                                                              : internalError,
                               dismissed.error().code == ErrorCode::cancelled ? staleRecordMessage
                                                                              : internalMessage);
        }
        service.observePostCommitSignal(service.emitStateChanged({"records"}));
        for (const auto &event : dismissed.value().closeEvents) {
            service.observePostCommitSignal(service.emitClose(event));
        }
        return sd_bus_reply_method_return(message, "t", dismissed.value().stateGeneration);
    }

    [[nodiscard]] static int handleInvokeAction(sd_bus_message *message, void *userData,
                                                sd_bus_error *error) {
        const int authorized = authorize(message, error);
        if (authorized < 0) {
            return authorized;
        }
        std::uint64_t recordId = 0U;
        std::uint64_t contentGeneration = 0U;
        const char *actionId = nullptr;
        if (sd_bus_message_read(message, "tts", &recordId, &contentGeneration, &actionId) < 0 ||
            actionId == nullptr) {
            return setBusError(error, invalidActionError, invalidActionMessage);
        }
        auto &service = from(userData);
        auto action = service.authority_.invokeAction(recordId, contentGeneration, actionId);
        if (!action) {
            const bool invalid = action.error().code == ErrorCode::invalid_argument;
            const bool stale = action.error().code == ErrorCode::cancelled;
            return setBusError(error,
                               invalid ? invalidActionError
                                       : (stale ? staleRecordError : actionUnavailableError),
                               invalid ? invalidActionMessage
                                       : (stale ? staleRecordMessage : actionUnavailableMessage));
        }
        if (!service.emitAction(action.value())) {
            return setBusError(error, internalError, internalMessage);
        }
        return sd_bus_reply_method_return(message, "t", service.authority_.generation());
    }

    [[nodiscard]] static int handleNameOwnerChanged(sd_bus_message *message, void *userData,
                                                    sd_bus_error *) {
        const char *name = nullptr;
        const char *oldOwner = nullptr;
        const char *newOwner = nullptr;
        if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 ||
            name == nullptr || oldOwner == nullptr || newOwner == nullptr) {
            return 0;
        }
        auto &service = from(userData);
        if (std::string_view{name} == settingsServiceName) {
            service.settings_generation_ = 0U;
            service.settings_refresh_requested_ = *newOwner != '\0';
        }
        if (*name == ':' && *oldOwner != '\0' && *newOwner == '\0') {
            auto lost = service.authority_.senderDisappeared(name);
            if (lost && lost.value().published) {
                service.observePostCommitSignal(service.emitStateChanged({"records"}));
            }
        }
        return 0;
    }

    [[nodiscard]] static int handleSettingsChanged(sd_bus_message *, void *userData,
                                                   sd_bus_error *) {
        from(userData).settings_refresh_requested_ = true;
        return 0;
    }

    [[nodiscard]] Result<void> emitStateChanged(const std::vector<std::string> &domains) {
        sdbus::Message signal;
        int result = sd_bus_message_new_signal(bus_, signal.put(), centerObjectPath,
                                               centerInterface, "StateChanged");
        if (result >= 0) {
            result = sd_bus_message_append(signal.get(), "t", authority_.generation());
        }
        if (result >= 0) {
            result = appendStringArray(signal.get(), domains);
        }
        if (result >= 0) {
            result = sd_bus_send(bus_, signal.get(), nullptr);
        }
        return result < 0 ? Result<void>::failure(serviceError(
                                ErrorCode::io_error, "Notification state could not be signaled."))
                          : Result<void>::success();
    }

    void observePostCommitSignal(const Result<void> &result) noexcept {
        post_commit_transport_.observe(static_cast<bool>(result));
    }

    [[nodiscard]] Result<void> emitClose(const CloseEvent &event) {
        sdbus::Message signal;
        const char *destination =
            event.destination && !event.destination->empty() ? event.destination->c_str() : nullptr;
        int result =
            destination == nullptr
                ? sd_bus_message_new_signal(bus_, signal.put(), standardObjectPath,
                                            standardInterface, "NotificationClosed")
                : sd_bus_message_new_signal_to(bus_, signal.put(), destination, standardObjectPath,
                                               standardInterface, "NotificationClosed");
        if (result >= 0) {
            result = sd_bus_message_append(signal.get(), "uu", event.notificationId,
                                           static_cast<std::uint32_t>(event.reason));
        }
        if (result >= 0) {
            result = sd_bus_send(bus_, signal.get(), nullptr);
        }
        return result < 0
                   ? Result<void>::failure(serviceError(
                         ErrorCode::io_error, "A notification close signal could not be sent."))
                   : Result<void>::success();
    }

    [[nodiscard]] Result<void> emitAction(const ActionEvent &event) {
        sdbus::Message signal;
        int result =
            sd_bus_message_new_signal_to(bus_, signal.put(), event.destination.c_str(),
                                         standardObjectPath, standardInterface, "ActionInvoked");
        if (result >= 0) {
            result = sd_bus_message_append(signal.get(), "us", event.notificationId,
                                           event.actionId.c_str());
        }
        if (result >= 0) {
            result = sd_bus_send(bus_, signal.get(), nullptr);
        }
        return result < 0
                   ? Result<void>::failure(serviceError(
                         ErrorCode::io_error, "A notification action signal could not be sent."))
                   : Result<void>::success();
    }

    void reportPersistenceDegradation() {
        if (authority_.takePersistenceDegradation() && !persistence_diagnostic_emitted_) {
            persistence_diagnostic_emitted_ = true;
            std::cerr << "prismdrake-notifyd: Persistent notification history is degraded.\n";
        }
    }

    sd_bus *bus_;
    NotificationAuthority authority_;
    sdbus::Slot standard_slot_;
    sdbus::Slot center_slot_;
    sdbus::Slot owner_slot_;
    sdbus::Slot settings_slot_;
    std::uint64_t settings_generation_{0U};
    bool settings_refresh_requested_{true};
    bool persistence_diagnostic_emitted_{false};
    PostCommitTransportGuard post_commit_transport_;
};

[[nodiscard]] bool busDisconnected(short revents) noexcept {
    return (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

} // namespace

Result<ServiceEpochOutcome> runServiceEpoch(NotificationAuthority authority,
                                            const volatile std::sig_atomic_t &stopRequested) {
    sdbus::Bus bus;
    if (sd_bus_open_user(bus.put()) < 0 || sd_bus_set_exit_on_disconnect(bus.get(), 0) < 0) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }

    NotificationService service{bus.get(), std::move(authority)};
    auto attached = service.attach();
    if (!attached) {
        return Result<ServiceEpochOutcome>::failure(attached.error());
    }
    const int standardName = sd_bus_request_name(bus.get(), standardServiceName, 0);
    if (standardName == -EEXIST) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::name_conflict);
    }
    if (standardName < 0) {
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }
    const int centerName = sd_bus_request_name(bus.get(), centerServiceName, 0);
    if (centerName == -EEXIST) {
        (void)sd_bus_release_name(bus.get(), standardServiceName);
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::name_conflict);
    }
    if (centerName < 0) {
        (void)sd_bus_release_name(bus.get(), standardServiceName);
        return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
    }

    (void)service.refreshSettings();
    while (stopRequested == 0) {
        int processed = 0;
        do {
            processed = sd_bus_process(bus.get(), nullptr);
        } while (processed > 0);
        if (processed < 0) {
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
        if (service.settingsRefreshRequested()) {
            (void)service.refreshSettings();
        }
        if (service.transportDegraded()) {
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
        auto timeouts = service.dispatchTimeouts();
        if (!timeouts) {
            return Result<ServiceEpochOutcome>::failure(timeouts.error());
        }
        if (service.transportDegraded()) {
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }

        pollfd descriptor{sd_bus_get_fd(bus.get()),
                          static_cast<short>(sd_bus_get_events(bus.get())), 0};
        const int pollResult = ::poll(&descriptor, 1U, service.pollTimeoutMilliseconds());
        if (pollResult < 0 && errno != EINTR) {
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
        if (busDisconnected(descriptor.revents)) {
            return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::disconnected);
        }
    }

    auto flushed = service.flushHistory();
    if (!flushed) {
        return Result<ServiceEpochOutcome>::failure(flushed.error());
    }
    (void)sd_bus_release_name(bus.get(), centerServiceName);
    (void)sd_bus_release_name(bus.get(), standardServiceName);
    (void)sd_bus_flush(bus.get());
    return Result<ServiceEpochOutcome>::success(ServiceEpochOutcome::stopped);
}

} // namespace prismdrake::notifyd
