#pragma once

#include "LauncherController.hpp"
#include "Result.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::shell::runtime {

struct ShellRuntimeOptions final {
    std::string display;
    launcher::controller::LauncherControllerOptions launcher;
};

/// Validates and preserves inherited environment entries byte-for-byte for direct execve use.
[[nodiscard]] foundation::Result<std::vector<std::string>>
validatedLaunchEnvironment(std::span<const std::string_view> inherited);

/// Captures and validates the bounded process environment needed by the shell runtime.
[[nodiscard]] foundation::Result<ShellRuntimeOptions> currentShellRuntimeOptions();

} // namespace prismdrake::shell::runtime
