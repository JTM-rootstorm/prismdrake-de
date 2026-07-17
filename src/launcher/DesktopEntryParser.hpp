#pragma once

#include "DesktopEntry.hpp"
#include "Result.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDesktopEntryFileBytes = 1024U * 1024U;
inline constexpr std::size_t maximumDesktopEntryLineBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopEntryLines = 16384U;
inline constexpr std::size_t maximumDesktopEntryGroups = 128U;
inline constexpr std::size_t maximumDesktopEntryEntries = 4096U;
inline constexpr std::size_t maximumDesktopEntryKeyBytes = 256U;
inline constexpr std::size_t maximumDesktopEntryValueBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopEntryListItems = 1024U;
inline constexpr std::size_t maximumDesktopEntryCodepoints = 256U * 1024U;
inline constexpr std::size_t maximumDesktopEntryValueCodepoints = 32U * 1024U;
inline constexpr std::size_t maximumDesktopEntryLocaleBytes = 128U;

/// Caller-supplied locale used only to resolve localized desktop-entry values.
struct DesktopEntryParseContext final {
    std::string messagesLocale;
};

/// Parses one bounded Desktop Entry Specification 1.5 Application document.
///
/// This function performs no filesystem access and retains no unknown fields.
/// Syntactically valid extension groups and X-* keys are accepted for format
/// extensibility, but round-trip preservation is not provided.
[[nodiscard]] foundation::Result<DesktopEntry>
parseDesktopEntry(std::string_view contents, const DesktopEntryParseContext &context);

} // namespace prismdrake::launcher
