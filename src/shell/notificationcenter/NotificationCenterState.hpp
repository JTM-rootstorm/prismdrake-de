#pragma once

#include "NotificationTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::shell::notificationcenter {

inline constexpr std::uint32_t notificationCenterStateSchemaVersion = 1U;

/// Exact private identity required by mutating notification-center methods.
///
/// This remains a C++ value type so 64-bit identifiers are never rounded through
/// QML's numeric representation.
struct NotificationRecordCommandIdentity final {
    std::uint64_t recordId{0U};
    std::uint64_t contentGeneration{0U};

    friend bool operator==(const NotificationRecordCommandIdentity &,
                           const NotificationRecordCommandIdentity &) = default;
};

struct NotificationCenterRecord final {
    std::uint64_t recordId{0U};
    std::uint64_t contentGeneration{0U};
    std::optional<std::uint32_t> notificationId;
    std::optional<std::string> applicationName;
    std::optional<std::string> desktopFileId;
    std::string summary;
    std::string body;
    notifications::ServiceUrgency urgency{notifications::ServiceUrgency::normal};
    std::uint64_t receivedAtUnixMilliseconds{0U};
    bool dismissible{false};
    bool transientPresentation{false};
    std::vector<notifications::ServiceAction> actions;

    friend bool operator==(const NotificationCenterRecord &,
                           const NotificationCenterRecord &) = default;
};

/// One complete immutable NotificationCenter1 owner-epoch publication.
struct NotificationCenterSnapshot final {
    std::uint64_t generation{0U};
    notifications::HistoryMode historyMode{notifications::HistoryMode::disabled};
    bool effectiveDoNotDisturb{false};
    std::optional<bool> runtimeDoNotDisturbOverride;
    std::vector<NotificationCenterRecord> records;
    std::string serializedJson;
};

using NotificationCenterSnapshotPtr = std::shared_ptr<const NotificationCenterSnapshot>;

} // namespace prismdrake::shell::notificationcenter
