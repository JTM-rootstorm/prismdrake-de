#include "DesktopEntryVisibility.hpp"

#include <algorithm>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<CurrentDesktopContext> failure(ErrorCode code, std::string message,
                                                    std::string recovery) {
    return Result<CurrentDesktopContext>::failure(
        Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool contains(const std::vector<std::string> &names,
                            std::string_view candidate) noexcept {
    return std::ranges::find(names, candidate) != names.end();
}

} // namespace

Result<CurrentDesktopContext> parseCurrentDesktopContext(std::string_view currentDesktop) {
    if (currentDesktop.size() > maximumCurrentDesktopBytes) {
        return failure(ErrorCode::too_large, "XDG_CURRENT_DESKTOP exceeds its byte limit.",
                       "Use a smaller bounded desktop-name list.");
    }
    if (currentDesktop.find('\0') != std::string_view::npos) {
        return failure(ErrorCode::invalid_environment,
                       "XDG_CURRENT_DESKTOP contains an invalid null byte.",
                       "Use a colon-separated list of desktop names.");
    }

    std::vector<std::string> names;
    std::size_t offset = 0U;
    while (offset <= currentDesktop.size()) {
        const auto separator = currentDesktop.find(':', offset);
        const auto length = separator == std::string_view::npos ? currentDesktop.size() - offset
                                                                : separator - offset;
        const auto component = currentDesktop.substr(offset, length);
        if (!component.empty()) {
            if (component.size() > maximumCurrentDesktopEntryBytes) {
                return failure(ErrorCode::too_large,
                               "XDG_CURRENT_DESKTOP contains an oversized desktop name.",
                               "Use shorter desktop names.");
            }
            if (names.size() >= maximumCurrentDesktopEntries) {
                return failure(ErrorCode::too_large,
                               "XDG_CURRENT_DESKTOP contains too many desktop names.",
                               "Use a smaller bounded desktop-name list.");
            }
            names.emplace_back(component);
        }

        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }

    return Result<CurrentDesktopContext>::success(CurrentDesktopContext{std::move(names)});
}

DesktopEntryVisibilityReason
evaluateDesktopEntryVisibility(const DesktopEntry &entry,
                               const CurrentDesktopContext &currentDesktop) noexcept {
    if (entry.hidden) {
        return DesktopEntryVisibilityReason::hiddenTombstone;
    }
    if (entry.noDisplay) {
        return DesktopEntryVisibilityReason::hiddenNoDisplay;
    }

    for (const auto &desktop : currentDesktop.names()) {
        if (entry.onlyShowIn && contains(*entry.onlyShowIn, desktop)) {
            return DesktopEntryVisibilityReason::visibleForCurrentDesktop;
        }
        if (entry.notShowIn && contains(*entry.notShowIn, desktop)) {
            return DesktopEntryVisibilityReason::hiddenForCurrentDesktop;
        }
    }

    if (entry.onlyShowIn) {
        return DesktopEntryVisibilityReason::hiddenWithoutOnlyShowInMatch;
    }
    return DesktopEntryVisibilityReason::visibleByDefault;
}

} // namespace prismdrake::launcher
