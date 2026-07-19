#include "NotificationHistory.hpp"

#include "AtomicFile.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::notifyd {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using notifications::ServiceAction;
using notifications::ServiceUrgency;
using OrderedJson = nlohmann::ordered_json;

constexpr std::size_t maximumJsonDepth = 8U;
constexpr std::size_t maximumJsonNodes = 16U * 1024U;
constexpr std::size_t maximumJsonKeyBytes = 64U;

class HistorySaxBoundary final : public nlohmann::json_sax<OrderedJson> {
  public:
    bool null() override { return consumeNode(); }
    bool boolean(bool) override { return consumeNode(); }
    bool number_integer(number_integer_t) override { return consumeNode(); }
    bool number_unsigned(number_unsigned_t) override { return consumeNode(); }
    bool number_float(number_float_t, const string_t &) override { return consumeNode(); }
    bool string(string_t &value) override {
        return value.size() <= notifications::maximumServiceBodyBytes && consumeNode();
    }
    bool binary(binary_t &) override { return false; }

    bool start_object(std::size_t elements) override {
        if (elements != nlohmann::detail::unknown_size() && elements > maximumJsonNodes) {
            return false;
        }
        if (!consumeNode() || containers_.size() >= maximumJsonDepth) {
            return false;
        }
        containers_.emplace_back(std::set<std::string>{});
        return true;
    }
    bool key(string_t &value) override {
        if (value.empty() || value.size() > maximumJsonKeyBytes || containers_.empty() ||
            !containers_.back()) {
            return false;
        }
        return containers_.back()->insert(value).second && consumeNode();
    }
    bool end_object() override { return endContainer(true); }

    bool start_array(std::size_t elements) override {
        if (elements != nlohmann::detail::unknown_size() && elements > maximumJsonNodes) {
            return false;
        }
        if (!consumeNode() || containers_.size() >= maximumJsonDepth) {
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
    [[nodiscard]] bool consumeNode() noexcept {
        if (nodes_ >= maximumJsonNodes) {
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

[[nodiscard]] Error historyError(ErrorCode code, std::string message) {
    return {code, std::move(message), "Retain the prior private history and recover safely."};
}

[[nodiscard]] std::string_view urgencyId(ServiceUrgency urgency) noexcept {
    switch (urgency) {
    case ServiceUrgency::low:
        return "low";
    case ServiceUrgency::normal:
        return "normal";
    case ServiceUrgency::critical:
        return "critical";
    }
    return "normal";
}

[[nodiscard]] std::optional<ServiceUrgency> parseUrgency(std::string_view value) noexcept {
    if (value == "low") {
        return ServiceUrgency::low;
    }
    if (value == "normal") {
        return ServiceUrgency::normal;
    }
    if (value == "critical") {
        return ServiceUrgency::critical;
    }
    return std::nullopt;
}

[[nodiscard]] bool hasExactKeys(const OrderedJson &object,
                                std::initializer_list<std::string_view> keys) {
    if (!object.is_object() || object.size() != keys.size()) {
        return false;
    }
    return std::ranges::all_of(
        keys, [&object](std::string_view key) { return object.contains(std::string{key}); });
}

[[nodiscard]] bool validUint64(const OrderedJson &value) noexcept {
    return value.is_number_unsigned() && value.get<std::uint64_t>() != 0U;
}

[[nodiscard]] Result<ServiceAction> parseAction(const OrderedJson &value) {
    if (!hasExactKeys(value, {"action_id", "label"}) || !value["action_id"].is_string() ||
        !value["label"].is_string()) {
        return Result<ServiceAction>::failure(
            historyError(ErrorCode::validation_error, "A notification history action is invalid."));
    }
    ServiceAction action{value["action_id"].get<std::string>(), value["label"].get<std::string>(),
                         false};
    notifications::ServiceNotificationInput input;
    input.sender = ":1.1";
    input.summary = "validation";
    input.actions.push_back(action);
    auto valid = notifications::validateServiceNotificationInput(input);
    if (!valid) {
        return Result<ServiceAction>::failure(historyError(
            valid.error().code, "A notification history action exceeds its contract."));
    }
    return Result<ServiceAction>::success(std::move(action));
}

[[nodiscard]] Result<NotificationRecord> parseRecord(const OrderedJson &value) {
    if (!hasExactKeys(value, {"record_id", "content_generation", "application_name",
                              "desktop_file_id", "summary", "body", "urgency",
                              "received_at_unix_ms", "dismissible", "actions"}) ||
        !validUint64(value["record_id"]) || !validUint64(value["content_generation"]) ||
        !(value["application_name"].is_null() || value["application_name"].is_string()) ||
        !(value["desktop_file_id"].is_null() || value["desktop_file_id"].is_string()) ||
        !value["summary"].is_string() || !value["body"].is_string() ||
        !value["urgency"].is_string() || !value["received_at_unix_ms"].is_number_unsigned() ||
        !value["dismissible"].is_boolean() || !value["actions"].is_array() ||
        value["actions"].size() > notifications::maximumServiceActions) {
        return Result<NotificationRecord>::failure(
            historyError(ErrorCode::validation_error, "A notification history record is invalid."));
    }

    const auto urgency = parseUrgency(value["urgency"].get_ref<const std::string &>());
    if (!urgency) {
        return Result<NotificationRecord>::failure(historyError(
            ErrorCode::validation_error, "A notification history urgency is invalid."));
    }

    std::vector<ServiceAction> actions;
    actions.reserve(value["actions"].size());
    for (const auto &actionValue : value["actions"]) {
        auto action = parseAction(actionValue);
        if (!action) {
            return Result<NotificationRecord>::failure(action.error());
        }
        actions.push_back(std::move(action).value());
    }

    notifications::ServiceNotificationInput validation;
    validation.sender = ":1.1";
    if (!value["application_name"].is_null()) {
        validation.applicationName = value["application_name"].get<std::string>();
    }
    if (!value["desktop_file_id"].is_null()) {
        validation.desktopFileId = value["desktop_file_id"].get<std::string>();
    }
    validation.summary = value["summary"].get<std::string>();
    validation.body = value["body"].get<std::string>();
    validation.actions = actions;
    validation.urgency = *urgency;
    auto valid = notifications::validateServiceNotificationInput(validation);
    if (!valid) {
        return Result<NotificationRecord>::failure(historyError(
            valid.error().code, "A notification history record exceeds its contract."));
    }

    return Result<NotificationRecord>::success(NotificationRecord{
        value["record_id"].get<std::uint64_t>(),
        value["content_generation"].get<std::uint64_t>(),
        std::nullopt,
        validation.applicationName,
        validation.desktopFileId,
        std::move(validation.summary),
        std::move(validation.body),
        *urgency,
        value["received_at_unix_ms"].get<std::uint64_t>(),
        value["dismissible"].get<bool>(),
        false,
        false,
        std::move(actions),
        std::nullopt,
        std::nullopt,
    });
}

[[nodiscard]] Result<void> ensurePrivateDirectory(const std::filesystem::path &path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return Result<void>::failure(historyError(
            ErrorCode::io_error, "A private notification directory could not be created."));
    }
    struct stat metadata = {};
    if (::lstat(path.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        S_ISLNK(metadata.st_mode) || metadata.st_uid != ::geteuid()) {
        return Result<void>::failure(historyError(ErrorCode::permission_denied,
                                                  "A private notification directory is unsafe."));
    }
    if (::chmod(path.c_str(), 0700) != 0) {
        return Result<void>::failure(
            historyError(ErrorCode::permission_denied,
                         "A private notification directory could not be secured."));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validatePrivateDirectory(const std::filesystem::path &path) {
    struct stat metadata = {};
    if (::lstat(path.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        S_ISLNK(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 0777U) != 0700U) {
        return Result<void>::failure(historyError(ErrorCode::permission_denied,
                                                  "A private notification directory is unsafe."));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateHistoryPathShape(const std::filesystem::path &path) {
    const auto notificationsDirectory = path.parent_path();
    const auto prismdrakeDirectory = notificationsDirectory.parent_path();
    const bool containsTraversal = std::ranges::any_of(
        path, [](const std::filesystem::path &component) { return component == ".."; });
    if (!path.is_absolute() || containsTraversal || path.filename() != "history-v1.json" ||
        notificationsDirectory.filename() != "notifications" ||
        prismdrakeDirectory.filename() != "prismdrake") {
        return Result<void>::failure(
            historyError(ErrorCode::invalid_argument,
                         "The notification history path is outside its XDG layout."));
    }
    auto prismdrake = validatePrivateDirectory(prismdrakeDirectory);
    if (!prismdrake) {
        return prismdrake;
    }
    return validatePrivateDirectory(notificationsDirectory);
}

[[nodiscard]] Result<std::string> readPrivateHistoryFile(const std::filesystem::path &path) {
    auto validPath = validateHistoryPathShape(path);
    if (!validPath) {
        return Result<std::string>::failure(validPath.error());
    }
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (descriptor < 0) {
        const auto code = errno == ENOENT
                              ? ErrorCode::not_found
                              : (errno == EACCES || errno == EPERM ? ErrorCode::permission_denied
                                                                   : ErrorCode::io_error);
        return Result<std::string>::failure(
            historyError(code, "The private notification history could not be opened."));
    }

    struct Descriptor final {
        int value;
        ~Descriptor() { (void)::close(value); }
    } owned{descriptor};
    struct stat metadata = {};
    if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != ::geteuid() || (metadata.st_mode & 0777U) != 0600U ||
        metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) >
            notifications::maximumNotificationHistoryBytes) {
        return Result<std::string>::failure(historyError(
            ErrorCode::permission_denied, "The private notification history file is unsafe."));
    }

    std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
        const auto amount = ::read(descriptor, bytes.data() + completed, bytes.size() - completed);
        if (amount > 0) {
            completed += static_cast<std::size_t>(amount);
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        return Result<std::string>::failure(historyError(
            ErrorCode::io_error, "The private notification history could not be read completely."));
    }
    std::array<char, 1U> extra{};
    if (::read(descriptor, extra.data(), extra.size()) != 0) {
        return Result<std::string>::failure(historyError(
            ErrorCode::too_large, "The private notification history changed during reading."));
    }
    return Result<std::string>::success(std::move(bytes));
}

[[nodiscard]] Result<void> ensureHistoryParent(const std::filesystem::path &path) {
    const auto notificationsDirectory = path.parent_path();
    const auto prismdrakeDirectory = notificationsDirectory.parent_path();
    if (notificationsDirectory.empty() || prismdrakeDirectory.empty() ||
        notificationsDirectory.filename() != "notifications" ||
        prismdrakeDirectory.filename() != "prismdrake") {
        return Result<void>::failure(
            historyError(ErrorCode::invalid_argument,
                         "The notification history path is outside its XDG layout."));
    }
    auto prismdrake = ensurePrivateDirectory(prismdrakeDirectory);
    if (!prismdrake) {
        return prismdrake;
    }
    return ensurePrivateDirectory(notificationsDirectory);
}

} // namespace

Result<std::string> serializeNotificationHistory(std::span<const NotificationRecord> records) {
    OrderedJson root = OrderedJson::object();
    root["schema_version"] = 1U;
    root["records"] = OrderedJson::array();
    std::set<std::uint64_t> recordIds;
    for (const auto &record : records) {
        if (record.notificationId || record.transientHint) {
            continue;
        }
        if (root["records"].size() >= notifications::maximumServiceRecords ||
            record.recordId == 0U || record.contentGeneration == 0U ||
            !recordIds.insert(record.recordId).second) {
            return Result<std::string>::failure(historyError(
                ErrorCode::validation_error, "Notification history cannot be serialized safely."));
        }
        notifications::ServiceNotificationInput validation;
        validation.sender = ":1.1";
        validation.applicationName = record.applicationName;
        validation.desktopFileId = record.desktopFileId;
        validation.summary = record.summary;
        validation.body = record.body;
        validation.actions = record.actions;
        validation.urgency = record.urgency;
        auto valid = notifications::validateServiceNotificationInput(validation);
        if (!valid) {
            return Result<std::string>::failure(historyError(
                valid.error().code, "Notification history cannot be serialized safely."));
        }

        OrderedJson actions = OrderedJson::array();
        for (const auto &action : record.actions) {
            actions.push_back({{"action_id", action.id}, {"label", action.label}});
        }
        root["records"].push_back({
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
            {"actions", std::move(actions)},
        });
    }
    auto encoded = root.dump();
    if (encoded.size() > notifications::maximumNotificationHistoryBytes) {
        return Result<std::string>::failure(
            historyError(ErrorCode::too_large, "Notification history exceeds its byte bound."));
    }
    return Result<std::string>::success(std::move(encoded));
}

Result<std::vector<NotificationRecord>> parseNotificationHistory(std::string_view jsonBytes) {
    if (jsonBytes.empty() || jsonBytes.size() > notifications::maximumNotificationHistoryBytes) {
        return Result<std::vector<NotificationRecord>>::failure(
            historyError(jsonBytes.size() > notifications::maximumNotificationHistoryBytes
                             ? ErrorCode::too_large
                             : ErrorCode::syntax_error,
                         "Notification history has an invalid byte length."));
    }

    HistorySaxBoundary boundary;
    if (!OrderedJson::sax_parse(jsonBytes.begin(), jsonBytes.end(), &boundary)) {
        return Result<std::vector<NotificationRecord>>::failure(historyError(
            ErrorCode::validation_error, "Notification history exceeds its parser boundaries."));
    }

    OrderedJson root;
    try {
        root = OrderedJson::parse(jsonBytes.begin(), jsonBytes.end());
    } catch (const nlohmann::json::exception &) {
        return Result<std::vector<NotificationRecord>>::failure(
            historyError(ErrorCode::syntax_error, "Notification history is not valid JSON."));
    }
    if (!hasExactKeys(root, {"schema_version", "records"}) ||
        !root["schema_version"].is_number_unsigned() ||
        root["schema_version"].get<std::uint64_t>() != 1U || !root["records"].is_array() ||
        root["records"].size() > notifications::maximumServiceRecords) {
        return Result<std::vector<NotificationRecord>>::failure(historyError(
            ErrorCode::validation_error, "Notification history does not match schema version 1."));
    }

    std::vector<NotificationRecord> records;
    std::set<std::uint64_t> recordIds;
    records.reserve(root["records"].size());
    for (const auto &recordValue : root["records"]) {
        auto record = parseRecord(recordValue);
        if (!record) {
            return Result<std::vector<NotificationRecord>>::failure(record.error());
        }
        if (!recordIds.insert(record.value().recordId).second) {
            return Result<std::vector<NotificationRecord>>::failure(historyError(
                ErrorCode::validation_error, "Notification history contains duplicate records."));
        }
        records.push_back(std::move(record).value());
    }
    return Result<std::vector<NotificationRecord>>::success(std::move(records));
}

Result<std::vector<NotificationRecord>> loadNotificationHistory(const std::filesystem::path &path) {
    auto bytes = readPrivateHistoryFile(path);
    if (!bytes) {
        return Result<std::vector<NotificationRecord>>::failure(bytes.error());
    }
    return parseNotificationHistory(bytes.value());
}

Result<void> writeNotificationHistory(const std::filesystem::path &path,
                                      std::span<const NotificationRecord> records) {
    auto encoded = serializeNotificationHistory(records);
    if (!encoded) {
        return Result<void>::failure(encoded.error());
    }
    auto directory = ensureHistoryParent(path);
    if (!directory) {
        return directory;
    }
    return foundation::writeFileAtomically(
        path, encoded.value(),
        foundation::AtomicWriteOptions{std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write});
}

} // namespace prismdrake::notifyd
