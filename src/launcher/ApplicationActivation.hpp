#pragma once

#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDbusApplicationNameBytes = 255U;

/// Standard org.freedesktop.Application identity derived from a desktop-file identifier.
struct DbusApplicationIdentity final {
    std::string serviceName;
    std::string objectPath;

    friend bool operator==(const DbusApplicationIdentity &,
                           const DbusApplicationIdentity &) = default;
};

/// Applies the Desktop Entry D-Bus activation transformation without bus access.
///
/// The `.desktop` suffix is removed for the well-known name. Dots become object-path separators,
/// and hyphens become underscores only in the object path. Identifiers that cannot form a valid
/// well-known D-Bus name fail closed instead of guessing an application identity.
[[nodiscard]] foundation::Result<DbusApplicationIdentity>
deriveDbusApplicationIdentity(const DesktopFileId &desktopFileId);

/// Validates an optional desktop-action identifier for typed ActivateAction dispatch.
[[nodiscard]] foundation::Result<std::optional<std::string>>
validateDbusDesktopAction(std::optional<std::string_view> actionId);

} // namespace prismdrake::launcher
