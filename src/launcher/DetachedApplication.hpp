#pragma once

#include "Cancellation.hpp"
#include "ProcessLaunch.hpp"
#include "Result.hpp"

#include <chrono>

namespace prismdrake::launcher {

/// Bounds the synchronous double-fork and execve confirmation handshake.
inline constexpr auto maximumDetachedApplicationHandshakeDuration = std::chrono::seconds{2};

/// Launches one validated application as a detached session without a shell.
///
/// This synchronous process boundary may block for the complete handshake duration and must run
/// only on an explicitly owned worker thread. Cancellation is observed only before the first fork
/// commit point. Success returns no PID or process ownership; the application is intentionally
/// outside Prismdrake's supervised process set.
[[nodiscard]] foundation::Result<void>
launchDetachedApplication(const ProcessLaunchPlan &plan,
                          const foundation::CancellationToken &cancellation);

} // namespace prismdrake::launcher
