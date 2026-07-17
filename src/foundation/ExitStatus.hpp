#pragma once

#include "Result.hpp"

namespace prismdrake::foundation {

/// Stable process exit statuses used at Prismdrake executable boundaries.
/// The explicit int representation matches main() and process-supervisor boundaries.
// NOLINTNEXTLINE(performance-enum-size)
enum class ExitStatus : int {
    success = 0,
    general_failure = 1,
    invalid_usage = 2,
    unavailable = 3,
    permission_denied = 4,
    resource_limit = 5,
    io_failure = 6,
    cancelled = 7,
};

[[nodiscard]] constexpr int processExitCode(ExitStatus status) noexcept {
    return static_cast<int>(status);
}

[[nodiscard]] constexpr ExitStatus exitStatusFor(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::syntax_error:
    case ErrorCode::validation_error:
        return ExitStatus::invalid_usage;
    case ErrorCode::invalid_environment:
    case ErrorCode::not_found:
    case ErrorCode::unsupported:
        return ExitStatus::unavailable;
    case ErrorCode::permission_denied:
        return ExitStatus::permission_denied;
    case ErrorCode::too_large:
        return ExitStatus::resource_limit;
    case ErrorCode::io_error:
    case ErrorCode::durability_uncertain:
        return ExitStatus::io_failure;
    case ErrorCode::cancelled:
        return ExitStatus::cancelled;
    }
    return ExitStatus::general_failure;
}

[[nodiscard]] constexpr ExitStatus exitStatusFor(const Error &error) noexcept {
    return exitStatusFor(error.code);
}

} // namespace prismdrake::foundation
