#pragma once

#include "DesktopEntry.hpp"
#include "DesktopExec.hpp"
#include "DesktopExecutable.hpp"
#include "Result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumProcessLaunchArguments = 256U;
inline constexpr std::size_t maximumProcessLaunchArgumentBytes = 64U * 1024U;
inline constexpr std::size_t maximumProcessLaunchEnvironmentEntries = 4096U;
inline constexpr std::size_t maximumProcessLaunchEnvironmentEntryBytes = 32U * 1024U;
inline constexpr std::size_t maximumProcessLaunchEnvironmentBytes = 1024U * 1024U;
inline constexpr std::size_t maximumProcessLaunchEnvelopeBytes = 1024U * 1024U;
inline constexpr std::size_t maximumProcessLaunchWorkingDirectoryBytes = 4095U;

/// Separately configured terminal command prefix.
///
/// argumentsBeforeCommand is an inert argument vector. The resolved application
/// executable and its remaining arguments are appended directly without a shell.
struct TerminalLaunchPolicy final {
    std::string executable;
    std::vector<std::string> argumentsBeforeCommand;

    friend bool operator==(const TerminalLaunchPolicy &, const TerminalLaunchPolicy &) = default;
};

/// Explicit process-planning inputs supplied by the session or shell boundary.
///
/// No member is read from ambient process state. Relative desktop-entry Path
/// values resolve against defaultWorkingDirectory. The environment is copied,
/// validated, and normalized to exactly one PWD entry in the resulting plan.
struct ProcessLaunchContext final {
    DesktopExecutableLookupContext executableLookup;
    std::filesystem::path defaultWorkingDirectory;
    std::vector<std::string> environment;
    std::optional<TerminalLaunchPolicy> terminal;

    friend bool operator==(const ProcessLaunchContext &, const ProcessLaunchContext &) = default;
};

/// Fully validated inert inputs for a later process-spawn boundary.
///
/// executable and workingDirectory are absolute lexical paths. argv is
/// nonempty and begins with executable. environment contains exactly one PWD
/// entry matching workingDirectory. This value performs no process operation.
struct ProcessLaunchPlan final {
    std::filesystem::path executable;
    std::vector<std::string> argv;
    std::filesystem::path workingDirectory;
    std::vector<std::string> environment;

    friend bool operator==(const ProcessLaunchPlan &, const ProcessLaunchPlan &) = default;
};

/// Resolves and validates one expanded desktop-entry invocation without
/// consulting ambient PATH, environment, or working-directory state.
///
/// Executable lookup is the only filesystem access. Working-directory paths
/// are validated lexically so the later spawn boundary remains authoritative
/// for chdir failures. D-Bus-activatable entries belong to their separate
/// activation path and are rejected.
[[nodiscard]] foundation::Result<ProcessLaunchPlan>
makeProcessLaunchPlan(const DesktopEntry &entry, const DesktopExecInvocation &invocation,
                      const ProcessLaunchContext &context);

} // namespace prismdrake::launcher
