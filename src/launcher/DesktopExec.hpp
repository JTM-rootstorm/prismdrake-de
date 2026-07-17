#pragma once

#include "DesktopEntry.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDesktopExecBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopExecArguments = 256U;
inline constexpr std::size_t maximumDesktopExecArgumentBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopExecArgumentVectorBytes = 1024U * 1024U;
inline constexpr std::size_t maximumDesktopExecTargets = 256U;
inline constexpr std::size_t maximumDesktopExecTargetBytes = 64U * 1024U;
inline constexpr std::size_t maximumDesktopExecTargetVectorBytes = 1024U * 1024U;
inline constexpr std::size_t maximumDesktopExecInvocations = 256U;
inline constexpr std::size_t maximumDesktopExecLocationBytes = 64U * 1024U;

/// The caller's explicitly typed file or URI inputs for Exec field expansion.
enum class DesktopExecTargetKind : std::uint8_t {
    none,
    localFiles,
    uris,
};

struct DesktopExecExpansionContext final {
    DesktopExecTargetKind targetKind{DesktopExecTargetKind::none};
    std::vector<std::string> targets;
    /// The discovered desktop file's local path or URI for %k.
    std::optional<std::string> desktopFileLocation;

    friend bool operator==(const DesktopExecExpansionContext &,
                           const DesktopExecExpansionContext &) = default;
};

/// One direct process argument vector. argv[0] is always present and nonempty.
struct DesktopExecInvocation final {
    std::vector<std::string> argv;

    friend bool operator==(const DesktopExecInvocation &, const DesktopExecInvocation &) = default;
};

/// Parses and expands an inert desktop-entry Exec value without filesystem or
/// process access.
///
/// The Desktop Entry general string escapes must already have been decoded by
/// parseDesktopEntry(). This function applies only Exec quoting and field-code
/// rules. It never invokes a shell and never rescans replacement text for field
/// codes. D-Bus-activatable entries belong to the separate D-Bus activation
/// path and are rejected here.
[[nodiscard]] foundation::Result<std::vector<DesktopExecInvocation>>
expandDesktopExec(const DesktopEntry &entry, const DesktopExecExpansionContext &context);

} // namespace prismdrake::launcher
