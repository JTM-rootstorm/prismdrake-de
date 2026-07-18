#pragma once

#include "Result.hpp"
#include "SessionEnvironment.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace prismdrake::session {

inline constexpr std::size_t maximumChildArgumentCount = 256U;
inline constexpr std::size_t maximumChildArgumentBytes = std::size_t{1024U} * 1024U;
inline constexpr std::size_t maximumChildExecutableBytes = 4096U;
/// Bounds the fork-to-exec handshake even if a child is stopped before execve.
inline constexpr auto maximumChildExecHandshakeDuration = std::chrono::seconds{2};

/// Explicit executable and argv passed directly to execve without a shell.
/// argv must include a non-empty argv[0].
struct ChildLaunch final {
    ChildLaunch() = default;
    ChildLaunch(std::filesystem::path executableValue, std::vector<std::string> argvValue,
                std::optional<int> inheritedDescriptorValue = std::nullopt)
        : executable(std::move(executableValue)), argv(std::move(argvValue)),
          inheritedDescriptor(inheritedDescriptorValue) {}

    std::filesystem::path executable;
    std::vector<std::string> argv;
    /// Optional close-on-exec descriptor made inheritable only in this exact forked child.
    std::optional<int> inheritedDescriptor;
};

enum class ChildExitKind : std::uint8_t {
    exited,
    signaled,
};

struct ChildExitStatus final {
    ChildExitKind kind;
    int value;
    bool coreDumped;

    friend bool operator==(const ChildExitStatus &, const ChildExitStatus &) = default;
};

/// PID-scoped ownership of one directly launched child.
///
/// Destruction intentionally neither signals nor waits for the child. The
/// lifecycle controller must make its shutdown and reaping policy explicit.
class ChildProcess final {
  public:
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ChildProcess(ChildProcess &&other) noexcept;
    ChildProcess &operator=(ChildProcess &&) = delete;
    ~ChildProcess() = default;

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] bool waitable() const noexcept { return pid_ > 0; }

    /// Returns an empty optional while this exact PID is still running.
    [[nodiscard]] foundation::Result<std::optional<ChildExitStatus>> waitNonBlocking();

    /// Waits for this exact PID, retrying waitpid after EINTR.
    [[nodiscard]] foundation::Result<ChildExitStatus> waitBlocking();

    /// Sends SIGTERM to this exact positive PID. ESRCH is treated as already stopped.
    [[nodiscard]] foundation::Result<void> sendTerminate() const;

    /// Sends SIGKILL to this exact positive PID. ESRCH is treated as already stopped.
    [[nodiscard]] foundation::Result<void> sendKill() const;

  private:
    friend foundation::Result<ChildProcess> launchChildProcess(const ChildLaunch &,
                                                               const PreparedSessionEnvironment &);

    explicit ChildProcess(pid_t pid) noexcept : pid_(pid) {}

    [[nodiscard]] foundation::Result<void> sendSignal(int signalNumber) const;
    [[nodiscard]] foundation::Result<ChildExitStatus> consumeWaitStatus(int status);

    pid_t pid_ = -1;
};

/// Forks and synchronously confirms execve through a close-on-exec error pipe.
/// launch.inheritedDescriptor is the only caller-selected close-on-exec descriptor made
/// inheritable in the exact child before execve.
[[nodiscard]] foundation::Result<ChildProcess>
launchChildProcess(const ChildLaunch &launch, const PreparedSessionEnvironment &environment);

} // namespace prismdrake::session
