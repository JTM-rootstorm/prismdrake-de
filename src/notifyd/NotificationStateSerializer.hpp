#pragma once

#include "NotificationAuthority.hpp"
#include "Result.hpp"

#include <string>

namespace prismdrake::notifyd {

/// Serializes one complete notification-center state schema version 1 document.
[[nodiscard]] foundation::Result<std::string>
serializeNotificationState(const NotificationAuthority &authority);

} // namespace prismdrake::notifyd
