#pragma once

#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDesktopExecutableNameBytes = 255U;
inline constexpr std::size_t maximumDesktopExecutableCandidateBytes = 4095U;
inline constexpr std::size_t maximumDesktopExecutableSearchPathBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopExecutableSearchPathComponents = 256U;

/// Explicit inputs for desktop-entry executable lookup.
///
/// Empty and relative PATH components are resolved against lookupBase. The
/// lookup never reads the ambient process working directory or PATH.
struct DesktopExecutableLookupContext final {
    std::string searchPath;
    std::filesystem::path lookupBase;

    friend bool operator==(const DesktopExecutableLookupContext &,
                           const DesktopExecutableLookupContext &) = default;
};

/// An absolute executable candidate validated as a regular X_OK file.
class ResolvedDesktopExecutable final {
  public:
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

    friend bool operator==(const ResolvedDesktopExecutable &,
                           const ResolvedDesktopExecutable &) = default;

  private:
    explicit ResolvedDesktopExecutable(std::filesystem::path path) : path_(std::move(path)) {}

    std::filesystem::path path_;

    friend foundation::Result<ResolvedDesktopExecutable>
    resolveDesktopExecutable(std::string_view, const DesktopExecutableLookupContext &);
};

enum class DesktopTryExecEligibilityReason : std::uint8_t {
    eligibleWithoutTryExec,
    eligibleExecutable,
    ineligibleMissing,
    ineligibleNotRegularFile,
    ineligibleNotExecutable,
};

/// Closed eligibility result for an optional desktop-entry TryExec value.
struct DesktopTryExecEligibility final {
    DesktopTryExecEligibilityReason reason;
    std::optional<std::filesystem::path> resolvedPath;

    friend bool operator==(const DesktopTryExecEligibility &,
                           const DesktopTryExecEligibility &) = default;
};

[[nodiscard]] constexpr bool isEligible(DesktopTryExecEligibilityReason reason) noexcept {
    return reason == DesktopTryExecEligibilityReason::eligibleWithoutTryExec ||
           reason == DesktopTryExecEligibilityReason::eligibleExecutable;
}

/// Resolves one absolute path or bare executable name without executing it.
///
/// Bare names are searched in PATH order. Symbolic links are followed for the
/// regular-file and X_OK checks, but the returned candidate remains lexical and
/// is not canonicalized. Missing and non-executable candidates are failures for
/// an actual Exec resolution; callers should re-resolve immediately before the
/// later process-spawn boundary.
[[nodiscard]] foundation::Result<ResolvedDesktopExecutable>
resolveDesktopExecutable(std::string_view executable,
                         const DesktopExecutableLookupContext &context);

/// Evaluates optional TryExec eligibility without treating ordinary absence,
/// a missing file, or a non-executable file as a parser failure.
[[nodiscard]] foundation::Result<DesktopTryExecEligibility>
evaluateDesktopTryExec(const std::optional<std::string> &tryExec,
                       const DesktopExecutableLookupContext &context);

} // namespace prismdrake::launcher
