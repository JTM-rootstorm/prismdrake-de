#pragma once

#include "DesktopEntry.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumCurrentDesktopBytes = 4096U;
inline constexpr std::size_t maximumCurrentDesktopEntries = 64U;
inline constexpr std::size_t maximumCurrentDesktopEntryBytes = 256U;

/// Ordered, bounded desktop names parsed from XDG_CURRENT_DESKTOP.
class CurrentDesktopContext final {
  public:
    [[nodiscard]] const std::vector<std::string> &names() const noexcept { return names_; }

    friend bool operator==(const CurrentDesktopContext &, const CurrentDesktopContext &) = default;

  private:
    explicit CurrentDesktopContext(std::vector<std::string> names) : names_(std::move(names)) {}

    std::vector<std::string> names_;

    friend foundation::Result<CurrentDesktopContext>
    parseCurrentDesktopContext(std::string_view currentDesktop);
};

/// Closed outcomes from desktop-entry launcher visibility evaluation.
enum class DesktopEntryVisibilityReason : std::uint8_t {
    visibleByDefault,
    visibleForCurrentDesktop,
    hiddenTombstone,
    hiddenNoDisplay,
    hiddenForCurrentDesktop,
    hiddenWithoutOnlyShowInMatch,
};

[[nodiscard]] foundation::Result<CurrentDesktopContext>
parseCurrentDesktopContext(std::string_view currentDesktop);

/// Evaluates display visibility without performing discovery or launch work.
///
/// Current desktop names are considered in their declared order. Empty
/// XDG_CURRENT_DESKTOP components are ignored by parseCurrentDesktopContext().
[[nodiscard]] DesktopEntryVisibilityReason
evaluateDesktopEntryVisibility(const DesktopEntry &entry,
                               const CurrentDesktopContext &currentDesktop) noexcept;

[[nodiscard]] constexpr bool isVisible(DesktopEntryVisibilityReason reason) noexcept {
    return reason == DesktopEntryVisibilityReason::visibleByDefault ||
           reason == DesktopEntryVisibilityReason::visibleForCurrentDesktop;
}

} // namespace prismdrake::launcher
