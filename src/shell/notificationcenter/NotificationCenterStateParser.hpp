#pragma once

#include "NotificationCenterState.hpp"
#include "Result.hpp"

#include <cstdint>
#include <string_view>

namespace prismdrake::shell::notificationcenter {

/// Parses one complete canonical schema-version-1 NotificationCenter1 reply.
///
/// The parser is toolkit-neutral, allocation-bounded, rejects duplicate keys and
/// identities, and requires the outer D-Bus generation to equal the embedded
/// generation. No untrusted content is reflected in an error.
[[nodiscard]] foundation::Result<NotificationCenterSnapshotPtr>
parseNotificationCenterState(std::uint64_t outerGeneration, std::string_view json);

} // namespace prismdrake::shell::notificationcenter
