#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace prismdrake::shellctl {

enum class Command : std::uint8_t {
    launcher_toggle,
    quick_settings_open,
    notifications_open,
    panel_focus,
    session_logout,
};

enum class ParseFailure : std::uint8_t { none, invalid_usage };

struct ParsedCommand final {
    Command command = Command::launcher_toggle;
    ParseFailure failure = ParseFailure::none;

    [[nodiscard]] bool valid() const noexcept { return failure == ParseFailure::none; }
};

[[nodiscard]] ParsedCommand parseCommand(std::span<const std::string_view> arguments) noexcept;
[[nodiscard]] std::string_view usageText() noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class ExitStatus : int {
    success = 0,
    internal_failure = 1,
    usage_error = 2,
    unavailable_service = 3,
    authorization_failure = 4,
    rejected_state = 5,
    timeout = 6,
};

[[nodiscard]] ExitStatus invoke(Command command) noexcept;

} // namespace prismdrake::shellctl
