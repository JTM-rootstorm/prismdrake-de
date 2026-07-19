#pragma once

#include "NotificationAuthority.hpp"
#include "Result.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::notifyd {

[[nodiscard]] foundation::Result<std::string>
serializeNotificationHistory(std::span<const NotificationRecord> records);

/// Parses one complete strict history document. Returned records are closed,
/// have no sender, and have every action disabled.
[[nodiscard]] foundation::Result<std::vector<NotificationRecord>>
parseNotificationHistory(std::string_view jsonBytes);

[[nodiscard]] foundation::Result<std::vector<NotificationRecord>>
loadNotificationHistory(const std::filesystem::path &path);

[[nodiscard]] foundation::Result<void>
writeNotificationHistory(const std::filesystem::path &path,
                         std::span<const NotificationRecord> records);

} // namespace prismdrake::notifyd
