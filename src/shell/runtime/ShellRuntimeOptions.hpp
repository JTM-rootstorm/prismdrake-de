#pragma once

#include "LauncherController.hpp"
#include "Result.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::shell::runtime {

/// Move-only owner of the exact child's private readiness event descriptor.
class SessionReadinessSignal final {
  public:
    SessionReadinessSignal() = default;
    SessionReadinessSignal(const SessionReadinessSignal &) = delete;
    SessionReadinessSignal &operator=(const SessionReadinessSignal &) = delete;
    SessionReadinessSignal(SessionReadinessSignal &&other) noexcept;
    SessionReadinessSignal &operator=(SessionReadinessSignal &&other) noexcept;
    ~SessionReadinessSignal();

    [[nodiscard]] bool pending() const noexcept { return descriptor_ >= 0; }
    [[nodiscard]] foundation::Result<void> publish();

  private:
    friend foundation::Result<SessionReadinessSignal>
        sessionReadinessSignalFromEnvironment(std::span<const std::string_view>);

    explicit SessionReadinessSignal(int descriptor) noexcept : descriptor_(descriptor) {}
    void close() noexcept;

    int descriptor_{-1};
};

struct ShellRuntimeOptions final {
    std::string display;
    launcher::controller::LauncherControllerOptions launcher;
    SessionReadinessSignal sessionReadiness;
};

/// Validates and preserves public inherited environment entries for direct application execve use.
/// The private session readiness descriptor is never propagated to launched applications.
[[nodiscard]] foundation::Result<std::vector<std::string>>
validatedLaunchEnvironment(std::span<const std::string_view> inherited);

/// Validates and adopts at most one private per-launch readiness descriptor.
[[nodiscard]] foundation::Result<SessionReadinessSignal>
sessionReadinessSignalFromEnvironment(std::span<const std::string_view> inherited);

/// Captures and validates the bounded process environment needed by the shell runtime.
[[nodiscard]] foundation::Result<ShellRuntimeOptions> currentShellRuntimeOptions();

} // namespace prismdrake::shell::runtime
