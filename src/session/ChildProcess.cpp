#include "ChildProcess.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace prismdrake::session {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Error invalidLaunch(std::string message, std::string recovery) {
    return {ErrorCode::invalid_argument, std::move(message), std::move(recovery)};
}

[[nodiscard]] Error processError(ErrorCode code, std::string message, std::string recovery) {
    return {code, std::move(message), std::move(recovery)};
}

[[nodiscard]] bool containsNull(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] Result<void> validateLaunch(const ChildLaunch &launch,
                                          const PreparedSessionEnvironment &environment) {
    const auto executable = launch.executable.native();
    if (executable.empty() || !launch.executable.is_absolute() || containsNull(executable) ||
        executable.size() > maximumChildExecutableBytes) {
        return Result<void>::failure(
            invalidLaunch("The child executable is not a valid bounded absolute path.",
                          "Use the absolute installed path of a Prismdrake component."));
    }
    if (launch.argv.empty() || launch.argv.size() > maximumChildArgumentCount ||
        launch.argv.front().empty()) {
        return Result<void>::failure(
            invalidLaunch("The child argument vector is empty or exceeds its entry limit.",
                          "Provide a non-empty argv[0] and a bounded argument vector."));
    }

    if (launch.inheritedDescriptor) {
        if (*launch.inheritedDescriptor < 3) {
            return Result<void>::failure(invalidLaunch(
                "The inherited child descriptor is invalid.",
                "Use one valid close-on-exec private descriptor above standard streams."));
        }
        int descriptorFlags = -1;
        do {
            descriptorFlags = ::fcntl(*launch.inheritedDescriptor, F_GETFD);
        } while (descriptorFlags < 0 && errno == EINTR);
        if (descriptorFlags < 0 || (descriptorFlags & FD_CLOEXEC) == 0) {
            return Result<void>::failure(invalidLaunch(
                "The inherited child descriptor is not safely bounded.",
                "Create the private descriptor close-on-exec before the exact child launch."));
        }
    }

    std::size_t argumentBytes = 0U;
    for (const auto &argument : launch.argv) {
        if (containsNull(argument) || argument.size() > maximumChildArgumentBytes - argumentBytes) {
            return Result<void>::failure(processError(
                ErrorCode::too_large, "The child argument vector exceeds its byte limit.",
                "Use fewer or smaller component arguments."));
        }
        argumentBytes += argument.size();
    }
    if (argumentBytes > maximumChildArgumentBytes) {
        return Result<void>::failure(
            processError(ErrorCode::too_large, "The child argument vector exceeds its byte limit.",
                         "Use fewer or smaller component arguments."));
    }

    if (environment.entries.empty() ||
        environment.entries.size() > maximumSessionEnvironmentEntries) {
        return Result<void>::failure(
            invalidLaunch("The prepared child environment is empty or exceeds its entry limit.",
                          "Prepare a bounded session environment before launching components."));
    }
    std::size_t environmentBytes = 0U;
    for (const auto &entry : environment.entries) {
        if (entry.empty() || containsNull(entry) || entry.find('=') == std::string::npos ||
            entry.front() == '=' || entry.size() > maximumSessionEnvironmentEntryBytes ||
            entry.size() > maximumSessionEnvironmentBytes - environmentBytes) {
            return Result<void>::failure(invalidLaunch(
                "The prepared child environment is malformed or exceeds its byte limit.",
                "Prepare a validated bounded session environment before launching components."));
        }
        environmentBytes += entry.size();
    }
    if (environmentBytes > maximumSessionEnvironmentBytes) {
        return Result<void>::failure(invalidLaunch(
            "The prepared child environment is malformed or exceeds its byte limit.",
            "Prepare a validated bounded session environment before launching components."));
    }
    return Result<void>::success();
}

[[nodiscard]] int closeOnExec(int descriptor) noexcept {
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        return -1;
    }
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result;
}

void closeDescriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
}

void waitForPidAfterLaunchFailure(pid_t pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void killAndReapAfterParentFailure(pid_t pid) noexcept {
    while (::kill(pid, SIGKILL) < 0 && errno == EINTR) {
    }
    waitForPidAfterLaunchFailure(pid);
}

[[nodiscard]] ErrorCode execErrorCode(int errorNumber) noexcept {
    switch (errorNumber) {
    case ENOENT:
    case ENOTDIR:
        return ErrorCode::not_found;
    case EACCES:
    case EPERM:
        return ErrorCode::permission_denied;
    case E2BIG:
        return ErrorCode::too_large;
    case ENOEXEC:
    case EINVAL:
        return ErrorCode::invalid_environment;
    default:
        return ErrorCode::io_error;
    }
}

[[nodiscard]] Result<void> waitForExecHandshake(int descriptor) {
    const auto deadline = std::chrono::steady_clock::now() + maximumChildExecHandshakeDuration;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return Result<void>::failure(processError(
                ErrorCode::io_error, "The child exec handshake exceeded its bounded deadline.",
                "Stop the child and review process resource and tracing state."));
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < deadline - now) {
            remaining += std::chrono::milliseconds{1};
        }
        pollfd descriptorState{descriptor, POLLIN, 0};
        const int result = ::poll(&descriptorState, 1U, static_cast<int>(remaining.count()));
        if (result > 0) {
            if ((descriptorState.revents & (POLLIN | POLLHUP)) != 0) {
                return Result<void>::success();
            }
            return Result<void>::failure(processError(
                ErrorCode::io_error, "The child exec handshake reported an invalid state.",
                "Stop the child and review bounded session diagnostics."));
        }
        if (result == 0) {
            return Result<void>::failure(processError(
                ErrorCode::io_error, "The child exec handshake exceeded its bounded deadline.",
                "Stop the child and review process resource and tracing state."));
        }
        if (errno != EINTR) {
            return Result<void>::failure(processError(
                ErrorCode::io_error, "The child exec handshake could not be monitored.",
                "Stop the child and review bounded session diagnostics."));
        }
    }
}

[[nodiscard]] Result<std::optional<int>> readExecError(int descriptor) {
    auto ready = waitForExecHandshake(descriptor);
    if (!ready) {
        return Result<std::optional<int>>::failure(ready.error());
    }
    std::array<unsigned char, sizeof(int)> bytes{};
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            if (offset == 0U) {
                return Result<std::optional<int>>::success(std::nullopt);
            }
            return Result<std::optional<int>>::failure(processError(
                ErrorCode::io_error, "The child exec handshake ended with an incomplete result.",
                "Retry the component launch and review bounded session diagnostics."));
        }
        if (errno == EINTR) {
            continue;
        }
        return Result<std::optional<int>>::failure(
            processError(ErrorCode::io_error, "The child exec handshake could not be read.",
                         "Retry the component launch and review bounded session diagnostics."));
    }

    int errorNumber = 0;
    static_assert(sizeof(errorNumber) == bytes.size());
    std::memcpy(&errorNumber, bytes.data(), bytes.size());
    return Result<std::optional<int>>::success(errorNumber);
}

void writeExecErrorAndExit(int descriptor, int errorNumber) noexcept {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&errorNumber);
    std::size_t offset = 0U;
    while (offset < sizeof(errorNumber)) {
        const auto count = ::write(descriptor, bytes + offset, sizeof(errorNumber) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::_exit(127);
}

} // namespace

ChildProcess::ChildProcess(ChildProcess &&other) noexcept : pid_(std::exchange(other.pid_, -1)) {}

Result<std::optional<ChildExitStatus>> ChildProcess::waitNonBlocking() {
    if (!waitable()) {
        return Result<std::optional<ChildExitStatus>>::failure(
            invalidLaunch("The child process has already been reaped or is not waitable.",
                          "Wait for each launched child exactly once."));
    }

    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return Result<std::optional<ChildExitStatus>>::success(std::nullopt);
    }
    if (result < 0) {
        if (errno == ECHILD) {
            pid_ = -1;
        }
        return Result<std::optional<ChildExitStatus>>::failure(
            processError(errno == ECHILD ? ErrorCode::not_found : ErrorCode::io_error,
                         "The child process status could not be collected.",
                         "Stop supervising this child and review bounded session diagnostics."));
    }
    auto decoded = consumeWaitStatus(status);
    if (!decoded) {
        return Result<std::optional<ChildExitStatus>>::failure(decoded.error());
    }
    return Result<std::optional<ChildExitStatus>>::success(std::move(decoded).value());
}

Result<ChildExitStatus> ChildProcess::waitBlocking() {
    if (!waitable()) {
        return Result<ChildExitStatus>::failure(
            invalidLaunch("The child process has already been reaped or is not waitable.",
                          "Wait for each launched child exactly once."));
    }

    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        if (errno == ECHILD) {
            pid_ = -1;
        }
        return Result<ChildExitStatus>::failure(
            processError(errno == ECHILD ? ErrorCode::not_found : ErrorCode::io_error,
                         "The child process status could not be collected.",
                         "Stop supervising this child and review bounded session diagnostics."));
    }
    return consumeWaitStatus(status);
}

Result<void> ChildProcess::sendTerminate() const { return sendSignal(SIGTERM); }

Result<void> ChildProcess::sendKill() const { return sendSignal(SIGKILL); }

Result<void> ChildProcess::sendSignal(int signalNumber) const {
    if (!waitable()) {
        return Result<void>::failure(
            invalidLaunch("The child process has already been reaped or is not signalable.",
                          "Signal only a currently supervised child."));
    }
    int result = -1;
    do {
        result = ::kill(pid_, signalNumber);
    } while (result < 0 && errno == EINTR);
    if (result == 0 || errno == ESRCH) {
        return Result<void>::success();
    }
    return Result<void>::failure(
        processError(errno == EPERM ? ErrorCode::permission_denied : ErrorCode::io_error,
                     "The child process could not be signaled.",
                     "Continue bounded shutdown and review session diagnostics."));
}

Result<ChildExitStatus> ChildProcess::consumeWaitStatus(int status) {
    pid_ = -1;
    if (WIFEXITED(status)) {
        return Result<ChildExitStatus>::success(
            ChildExitStatus{ChildExitKind::exited, WEXITSTATUS(status), false});
    }
    if (WIFSIGNALED(status)) {
        bool coreDumped = false;
#ifdef WCOREDUMP
        coreDumped = WCOREDUMP(status) != 0;
#endif
        return Result<ChildExitStatus>::success(
            ChildExitStatus{ChildExitKind::signaled, WTERMSIG(status), coreDumped});
    }
    return Result<ChildExitStatus>::failure(
        processError(ErrorCode::io_error, "The child process returned an unsupported wait status.",
                     "Stop supervising this child and review bounded session diagnostics."));
}

Result<ChildProcess> launchChildProcess(const ChildLaunch &launch,
                                        const PreparedSessionEnvironment &environment) {
    auto validated = validateLaunch(launch, environment);
    if (!validated) {
        return Result<ChildProcess>::failure(validated.error());
    }

    const std::string executable = launch.executable.native();
    std::vector<char *> argv;
    argv.reserve(launch.argv.size() + 1U);
    for (const auto &argument : launch.argv) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<char *> envp;
    envp.reserve(environment.entries.size() + 1U);
    for (const auto &entry : environment.entries) {
        envp.push_back(const_cast<char *>(entry.c_str()));
    }
    envp.push_back(nullptr);

    std::array<int, 2U> errorPipe{-1, -1};
    int pipeResult = -1;
    do {
        pipeResult = ::pipe(errorPipe.data());
    } while (pipeResult < 0 && errno == EINTR);
    if (pipeResult < 0 || closeOnExec(errorPipe[0]) < 0 || closeOnExec(errorPipe[1]) < 0) {
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        return Result<ChildProcess>::failure(
            processError(ErrorCode::io_error, "The child exec handshake could not be created.",
                         "Retry the component launch and review process resource limits."));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        return Result<ChildProcess>::failure(
            processError(ErrorCode::io_error, "The child process could not be created.",
                         "Retry the component launch and review process resource limits."));
    }
    if (pid == 0) {
        closeDescriptor(errorPipe[0]);
        if (launch.inheritedDescriptor) {
            int flags = -1;
            do {
                flags = ::fcntl(*launch.inheritedDescriptor, F_GETFD);
            } while (flags < 0 && errno == EINTR);
            int inherited = -1;
            do {
                inherited =
                    flags < 0 ? -1
                              : ::fcntl(*launch.inheritedDescriptor, F_SETFD, flags & ~FD_CLOEXEC);
            } while (inherited < 0 && errno == EINTR);
            if (inherited < 0) {
                const int errorNumber = errno;
                writeExecErrorAndExit(errorPipe[1], errorNumber);
            }
        }
        ::execve(executable.c_str(), argv.data(), envp.data());
        const int errorNumber = errno;
        writeExecErrorAndExit(errorPipe[1], errorNumber);
    }

    closeDescriptor(errorPipe[1]);
    auto execError = readExecError(errorPipe[0]);
    closeDescriptor(errorPipe[0]);
    if (!execError) {
        killAndReapAfterParentFailure(pid);
        return Result<ChildProcess>::failure(execError.error());
    }
    const int execErrorNumber = execError.value().value_or(0);
    if (execErrorNumber != 0) {
        waitForPidAfterLaunchFailure(pid);
        return Result<ChildProcess>::failure(
            processError(execErrorCode(execErrorNumber), "The child process could not execute.",
                         "Verify the installed component executable and its permissions."));
    }
    return Result<ChildProcess>::success(ChildProcess{pid});
}

} // namespace prismdrake::session
