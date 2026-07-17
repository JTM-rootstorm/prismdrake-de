#include "DetachedApplication.hpp"

#include "DesktopExecutable.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class LaunchStage : std::uint32_t {
    brokerReady = 1U,
    createSession = 2U,
    secondFork = 3U,
    resetSignalMask = 4U,
    resetSignalDisposition = 5U,
    workingDirectory = 6U,
    standardIo = 7U,
    execute = 8U,
};

struct WireRecord final {
    std::uint32_t stage;
    std::int32_t errorNumber;
};

static_assert(sizeof(WireRecord) == 8U);
static_assert(std::is_trivially_copyable_v<WireRecord>);

struct PreparedLaunch final {
    std::string executable;
    std::string workingDirectory;
    std::vector<char *> argv;
    std::vector<char *> environment;
    int maximumDescriptorExclusive;
};

[[nodiscard]] Error processError(ErrorCode code, std::string message, std::string recovery) {
    return {code, std::move(message), std::move(recovery)};
}

[[nodiscard]] Result<void> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<void>::failure(processError(code, std::move(message), std::move(recovery)));
}

[[nodiscard]] bool containsNull(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool addWithin(std::size_t &total, std::size_t amount, std::size_t limit) noexcept {
    if (amount > limit - total) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] Result<int> maximumOpenDescriptor() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) < 0 || limit.rlim_cur == RLIM_INFINITY ||
        limit.rlim_cur > static_cast<rlim_t>(std::numeric_limits<int>::max())) {
        return Result<int>::failure(processError(
            ErrorCode::unsupported, "The process descriptor limit cannot be bounded safely.",
            "Use a finite RLIMIT_NOFILE within the supported integer descriptor range."));
    }
    return Result<int>::success(static_cast<int>(limit.rlim_cur));
}

[[nodiscard]] Result<PreparedLaunch> prepareLaunch(const ProcessLaunchPlan &plan) {
    const auto executable = plan.executable.native();
    if (executable.empty() || !plan.executable.is_absolute() || containsNull(executable) ||
        plan.executable.lexically_normal() != plan.executable) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::invalid_argument, "The detached application executable is invalid.",
            "Use one bounded normalized NUL-free absolute executable path."));
    }
    if (executable.size() > maximumDesktopExecutableCandidateBytes) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::too_large, "The detached application executable path is too large.",
            "Use a smaller normalized absolute executable path."));
    }

    const auto workingDirectory = plan.workingDirectory.native();
    if (workingDirectory.empty() || !plan.workingDirectory.is_absolute() ||
        containsNull(workingDirectory) ||
        plan.workingDirectory.lexically_normal() != plan.workingDirectory) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::invalid_argument, "The detached application working directory is invalid.",
            "Use one bounded normalized NUL-free absolute working directory."));
    }
    if (workingDirectory.size() > maximumProcessLaunchWorkingDirectoryBytes) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::too_large, "The detached application working directory is too large.",
            "Use a smaller normalized absolute working directory."));
    }

    if (plan.argv.empty() || plan.argv.front() != executable) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::invalid_argument, "The detached application argument vector is invalid.",
            "Use a bounded vector whose argv[0] exactly matches the executable path."));
    }
    if (plan.argv.size() > maximumProcessLaunchArguments) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::too_large, "The detached application argument vector is too large.",
            "Use fewer argument values."));
    }

    std::size_t envelopeBytes = 0U;
    const auto pointerEntries = plan.argv.size() + plan.environment.size() + 2U;
    if (pointerEntries < plan.argv.size() ||
        pointerEntries > std::numeric_limits<std::size_t>::max() / sizeof(char *) ||
        !addWithin(envelopeBytes, pointerEntries * sizeof(char *),
                   maximumProcessLaunchEnvelopeBytes)) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::too_large, "The detached application launch envelope is too large.",
            "Use fewer arguments and environment entries."));
    }

    for (const auto &argument : plan.argv) {
        if (containsNull(argument)) {
            return Result<PreparedLaunch>::failure(processError(
                ErrorCode::invalid_argument, "The detached application argument vector is invalid.",
                "Use only NUL-free argument values."));
        }
        if (argument.size() > maximumProcessLaunchArgumentBytes ||
            !addWithin(envelopeBytes, argument.size() + 1U, maximumProcessLaunchEnvelopeBytes)) {
            return Result<PreparedLaunch>::failure(processError(
                ErrorCode::too_large, "The detached application argument vector is too large.",
                "Use fewer or smaller argument values."));
        }
    }

    if (plan.environment.empty()) {
        return Result<PreparedLaunch>::failure(processError(
            ErrorCode::invalid_environment, "The detached application environment is invalid.",
            "Use one bounded explicit environment containing exactly one matching PWD entry."));
    }
    if (plan.environment.size() > maximumProcessLaunchEnvironmentEntries) {
        return Result<PreparedLaunch>::failure(
            processError(ErrorCode::too_large, "The detached application environment is too large.",
                         "Use fewer explicit environment entries."));
    }
    std::unordered_set<std::string_view> names;
    names.reserve(plan.environment.size());
    std::size_t environmentBytes = 0U;
    std::size_t pwdEntries = 0U;
    for (const auto &entry : plan.environment) {
        const auto separator = entry.find('=');
        if (entry.empty() || containsNull(entry) || separator == std::string::npos ||
            separator == 0U ||
            !names.insert(std::string_view{entry}.substr(0U, separator)).second) {
            return Result<PreparedLaunch>::failure(processError(
                ErrorCode::invalid_environment, "The detached application environment is invalid.",
                "Use unique nonempty NUL-free NAME=value entries."));
        }
        if (entry.size() > maximumProcessLaunchEnvironmentEntryBytes ||
            !addWithin(environmentBytes, entry.size() + 1U, maximumProcessLaunchEnvironmentBytes) ||
            !addWithin(envelopeBytes, entry.size() + 1U, maximumProcessLaunchEnvelopeBytes)) {
            return Result<PreparedLaunch>::failure(processError(
                ErrorCode::too_large, "The detached application environment is too large.",
                "Use fewer or smaller explicit environment entries."));
        }
        const std::string_view name{entry.data(), separator};
        if (name == "PWD") {
            ++pwdEntries;
            if (std::string_view{entry}.substr(separator + 1U) != workingDirectory) {
                return Result<PreparedLaunch>::failure(processError(
                    ErrorCode::invalid_environment,
                    "The detached application PWD entry does not match its working directory.",
                    "Use exactly one PWD entry matching the explicit working directory."));
            }
        }
    }
    if (pwdEntries != 1U) {
        return Result<PreparedLaunch>::failure(
            processError(ErrorCode::invalid_environment,
                         "The detached application environment has no matching PWD entry.",
                         "Use exactly one PWD entry matching the explicit working directory."));
    }

    auto descriptorLimit = maximumOpenDescriptor();
    if (!descriptorLimit) {
        return Result<PreparedLaunch>::failure(descriptorLimit.error());
    }

    PreparedLaunch prepared{executable, workingDirectory, {}, {}, descriptorLimit.value()};
    prepared.argv.reserve(plan.argv.size() + 1U);
    for (const auto &argument : plan.argv) {
        prepared.argv.push_back(const_cast<char *>(argument.c_str()));
    }
    prepared.argv.push_back(nullptr);
    prepared.environment.reserve(plan.environment.size() + 1U);
    for (const auto &entry : plan.environment) {
        prepared.environment.push_back(const_cast<char *>(entry.c_str()));
    }
    prepared.environment.push_back(nullptr);
    return Result<PreparedLaunch>::success(std::move(prepared));
}

void closeDescriptor(int descriptor) noexcept {
    if (descriptor >= 0) {
        (void)::close(descriptor);
    }
}

[[nodiscard]] int duplicateAtLeastStandardDescriptors(int descriptor) noexcept {
    if (descriptor >= 3) {
        return descriptor;
    }
    int duplicate = -1;
    do {
        duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    } while (duplicate < 0 && errno == EINTR);
    closeDescriptor(descriptor);
    return duplicate;
}

[[nodiscard]] bool createCloexecPipe(std::array<int, 2U> &descriptors) noexcept {
    descriptors = {-1, -1};
    int result = -1;
    do {
        result = ::pipe2(descriptors.data(), O_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return false;
    }
    descriptors[0] = duplicateAtLeastStandardDescriptors(descriptors[0]);
    descriptors[1] = duplicateAtLeastStandardDescriptors(descriptors[1]);
    if (descriptors[0] < 0 || descriptors[1] < 0) {
        closeDescriptor(descriptors[0]);
        closeDescriptor(descriptors[1]);
        descriptors = {-1, -1};
        return false;
    }
    return true;
}

[[nodiscard]] int openNullDevice() noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return -1;
    }
    return duplicateAtLeastStandardDescriptors(descriptor);
}

[[noreturn]] void writeRecordAndExit(int descriptor, LaunchStage stage, int errorNumber) noexcept {
    const WireRecord record{static_cast<std::uint32_t>(stage), errorNumber};
    const auto *bytes = reinterpret_cast<const unsigned char *>(&record);
    std::size_t offset = 0U;
    while (offset < sizeof(record)) {
        const auto count = ::write(descriptor, bytes + offset, sizeof(record) - offset);
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

[[nodiscard]] bool validStage(std::uint32_t stage) noexcept {
    return stage >= static_cast<std::uint32_t>(LaunchStage::brokerReady) &&
           stage <= static_cast<std::uint32_t>(LaunchStage::execute);
}

[[nodiscard]] int remainingMilliseconds(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining < deadline - now) {
        remaining += std::chrono::milliseconds{1};
    }
    return remaining.count() > std::numeric_limits<int>::max()
               ? std::numeric_limits<int>::max()
               : static_cast<int>(remaining.count());
}

[[nodiscard]] Result<WireRecord> readWireRecord(int descriptor,
                                                std::chrono::steady_clock::time_point deadline,
                                                std::string_view timeoutMessage,
                                                std::string_view failureMessage) {
    std::array<unsigned char, sizeof(WireRecord)> bytes{};
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto timeout = remainingMilliseconds(deadline);
        if (timeout == 0) {
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{timeoutMessage},
                             "Stop the detached launch and review bounded process diagnostics."));
        }
        pollfd state{descriptor, POLLIN, 0};
        const int pollResult = ::poll(&state, 1U, timeout);
        if (pollResult == 0) {
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{timeoutMessage},
                             "Stop the detached launch and review bounded process diagnostics."));
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{failureMessage},
                             "Retry the detached launch and review bounded process diagnostics."));
        }
        if ((state.revents & (POLLIN | POLLHUP)) == 0) {
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{failureMessage},
                             "Retry the detached launch and review bounded process diagnostics."));
        }
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{failureMessage},
                             "Retry the detached launch and review bounded process diagnostics."));
        }
        if (errno != EINTR) {
            return Result<WireRecord>::failure(
                processError(ErrorCode::io_error, std::string{failureMessage},
                             "Retry the detached launch and review bounded process diagnostics."));
        }
    }

    WireRecord record{};
    std::memcpy(&record, bytes.data(), bytes.size());
    if (!validStage(record.stage) || record.errorNumber < 0) {
        return Result<WireRecord>::failure(
            processError(ErrorCode::io_error, std::string{failureMessage},
                         "Retry the detached launch and review bounded process diagnostics."));
    }
    return Result<WireRecord>::success(record);
}

[[nodiscard]] Result<std::optional<WireRecord>>
readExecOutcome(int descriptor, std::chrono::steady_clock::time_point deadline) {
    std::array<unsigned char, sizeof(WireRecord)> bytes{};
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto timeout = remainingMilliseconds(deadline);
        if (timeout == 0) {
            return Result<std::optional<WireRecord>>::failure(processError(
                ErrorCode::io_error,
                "The detached application exec handshake exceeded its bounded deadline.",
                "Stop the detached launch and review bounded process diagnostics."));
        }
        pollfd state{descriptor, POLLIN, 0};
        const int pollResult = ::poll(&state, 1U, timeout);
        if (pollResult == 0) {
            return Result<std::optional<WireRecord>>::failure(processError(
                ErrorCode::io_error,
                "The detached application exec handshake exceeded its bounded deadline.",
                "Stop the detached launch and review bounded process diagnostics."));
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<std::optional<WireRecord>>::failure(
                processError(ErrorCode::io_error, "The detached application exec handshake failed.",
                             "Retry the detached launch and review bounded process diagnostics."));
        }
        if ((state.revents & (POLLIN | POLLHUP)) == 0) {
            return Result<std::optional<WireRecord>>::failure(
                processError(ErrorCode::io_error, "The detached application exec handshake failed.",
                             "Retry the detached launch and review bounded process diagnostics."));
        }
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            if (offset == 0U) {
                return Result<std::optional<WireRecord>>::success(std::nullopt);
            }
            return Result<std::optional<WireRecord>>::failure(processError(
                ErrorCode::io_error, "The detached application exec handshake was incomplete.",
                "Retry the detached launch and review bounded process diagnostics."));
        }
        if (errno != EINTR) {
            return Result<std::optional<WireRecord>>::failure(
                processError(ErrorCode::io_error, "The detached application exec handshake failed.",
                             "Retry the detached launch and review bounded process diagnostics."));
        }
    }

    WireRecord record{};
    std::memcpy(&record, bytes.data(), bytes.size());
    if (!validStage(record.stage) ||
        record.stage == static_cast<std::uint32_t>(LaunchStage::brokerReady) ||
        record.errorNumber <= 0) {
        return Result<std::optional<WireRecord>>::failure(processError(
            ErrorCode::io_error, "The detached application exec handshake was malformed.",
            "Retry the detached launch and review bounded process diagnostics."));
    }
    return Result<std::optional<WireRecord>>::success(record);
}

void signalExact(int target, int signalNumber) noexcept {
    int result = -1;
    do {
        result = ::kill(target, signalNumber);
    } while (result < 0 && errno == EINTR);
}

void waitExact(pid_t pid) noexcept {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

void killGroupAndReapBroker(pid_t broker) noexcept {
    if (broker <= 0) {
        return;
    }
    signalExact(-broker, SIGKILL);
    signalExact(broker, SIGKILL);
    waitExact(broker);
}

[[nodiscard]] Result<void> reapBroker(pid_t broker,
                                      std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        int status = 0;
        const auto result = ::waitpid(broker, &status, WNOHANG);
        if (result == broker) {
            if (WIFEXITED(status)) {
                return Result<void>::success();
            }
            return failure(ErrorCode::io_error, "The detached launch broker ended unexpectedly.",
                           "Retry the launch and review bounded process diagnostics.");
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                // A process-wide SIGCHLD policy may have reaped the broker first. It is no
                // longer waitable and therefore cannot remain as a zombie owned by this process.
                return Result<void>::success();
            }
            return failure(ErrorCode::io_error, "The detached launch broker could not be reaped.",
                           "Retry the launch and review bounded process diagnostics.");
        }
        const auto timeout = remainingMilliseconds(deadline);
        if (timeout == 0) {
            return failure(ErrorCode::io_error,
                           "The detached launch broker exceeded its bounded deadline.",
                           "Stop the detached launch and review bounded process diagnostics.");
        }
        const int delay = std::min(timeout, 5);
        while (::poll(nullptr, 0U, delay) < 0 && errno == EINTR) {
        }
    }
}

[[nodiscard]] ErrorCode errorCodeForErrno(int errorNumber) noexcept {
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

[[nodiscard]] Error errorForRecord(const WireRecord &record) {
    const auto stage = static_cast<LaunchStage>(record.stage);
    if (stage == LaunchStage::workingDirectory) {
        return processError(errorCodeForErrno(record.errorNumber),
                            "The detached application working directory could not be entered.",
                            "Verify the configured working directory and its permissions.");
    }
    if (stage == LaunchStage::execute) {
        return processError(errorCodeForErrno(record.errorNumber),
                            "The detached application could not execute.",
                            "Verify the installed executable and its permissions.");
    }
    if (stage == LaunchStage::createSession || stage == LaunchStage::secondFork) {
        return processError(ErrorCode::io_error,
                            "The detached application session could not be created.",
                            "Retry the launch and review process resource limits.");
    }
    return processError(ErrorCode::io_error,
                        "The detached application process setup could not be completed.",
                        "Retry the launch and review bounded process diagnostics.");
}

void resetSignalsOrExit(int errorDescriptor) noexcept {
    sigset_t emptyMask{};
    if (::sigemptyset(&emptyMask) < 0 || ::sigprocmask(SIG_SETMASK, &emptyMask, nullptr) < 0) {
        const int errorNumber = errno;
        writeRecordAndExit(errorDescriptor, LaunchStage::resetSignalMask, errorNumber);
    }

    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    (void)::sigemptyset(&defaultAction.sa_mask);
    defaultAction.sa_flags = 0;
    for (int signalNumber = 1; signalNumber < NSIG; ++signalNumber) {
        if (signalNumber == SIGKILL || signalNumber == SIGSTOP) {
            continue;
        }
        if (::sigaction(signalNumber, &defaultAction, nullptr) < 0 && errno != EINVAL) {
            const int errorNumber = errno;
            writeRecordAndExit(errorDescriptor, LaunchStage::resetSignalDisposition, errorNumber);
        }
    }
}

void prepareStandardIoOrExit(int nullDescriptor, int errorDescriptor) noexcept {
    for (int descriptor = STDIN_FILENO; descriptor <= STDERR_FILENO; ++descriptor) {
        if (::dup2(nullDescriptor, descriptor) < 0) {
            const int errorNumber = errno;
            writeRecordAndExit(errorDescriptor, LaunchStage::standardIo, errorNumber);
        }
    }
}

void closeUnintendedDescriptors(int maximumDescriptorExclusive, int errorDescriptor) noexcept {
    for (int descriptor = 3; descriptor < maximumDescriptorExclusive; ++descriptor) {
        if (descriptor != errorDescriptor) {
            (void)::close(descriptor);
        }
    }
}

#if defined(PRISMDRAKE_DETACHED_APPLICATION_TEST_HOOK)
extern "C" void prismdrakeDetachedApplicationPreExecTestHook() noexcept;
#endif

[[noreturn]] void runDetachedChild(const PreparedLaunch &prepared, int nullDescriptor,
                                   int errorDescriptor, int resultRead, int resultWrite) noexcept {
    closeDescriptor(resultRead);
    closeDescriptor(resultWrite);
    resetSignalsOrExit(errorDescriptor);
    if (::chdir(prepared.workingDirectory.c_str()) < 0) {
        const int errorNumber = errno;
        writeRecordAndExit(errorDescriptor, LaunchStage::workingDirectory, errorNumber);
    }
    prepareStandardIoOrExit(nullDescriptor, errorDescriptor);
    closeUnintendedDescriptors(prepared.maximumDescriptorExclusive, errorDescriptor);

#if defined(PRISMDRAKE_DETACHED_APPLICATION_TEST_HOOK)
    prismdrakeDetachedApplicationPreExecTestHook();
#endif

    ::execve(prepared.executable.c_str(), prepared.argv.data(), prepared.environment.data());
    const int errorNumber = errno;
    writeRecordAndExit(errorDescriptor, LaunchStage::execute, errorNumber);
}

[[noreturn]] void runBroker(const PreparedLaunch &prepared, int nullDescriptor,
                            const std::array<int, 2U> &errorPipe,
                            const std::array<int, 2U> &resultPipe) noexcept {
    closeDescriptor(errorPipe[0]);
    closeDescriptor(resultPipe[0]);
    if (::setsid() < 0) {
        const int errorNumber = errno;
        closeDescriptor(errorPipe[1]);
        writeRecordAndExit(resultPipe[1], LaunchStage::createSession, errorNumber);
    }

    const pid_t detached = ::fork();
    if (detached < 0) {
        const int errorNumber = errno;
        closeDescriptor(errorPipe[1]);
        writeRecordAndExit(resultPipe[1], LaunchStage::secondFork, errorNumber);
    }
    if (detached == 0) {
        runDetachedChild(prepared, nullDescriptor, errorPipe[1], resultPipe[0], resultPipe[1]);
    }

    closeDescriptor(errorPipe[1]);
    const WireRecord ready{static_cast<std::uint32_t>(LaunchStage::brokerReady), 0};
    const auto *bytes = reinterpret_cast<const unsigned char *>(&ready);
    std::size_t offset = 0U;
    while (offset < sizeof(ready)) {
        const auto count = ::write(resultPipe[1], bytes + offset, sizeof(ready) - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::_exit(offset == sizeof(ready) ? 0 : 127);
}

} // namespace

Result<void> launchDetachedApplication(const ProcessLaunchPlan &plan,
                                       const foundation::CancellationToken &cancellation) {
    auto prepared = prepareLaunch(plan);
    if (!prepared) {
        return Result<void>::failure(prepared.error());
    }

    std::array<int, 2U> errorPipe{-1, -1};
    std::array<int, 2U> resultPipe{-1, -1};
    const int nullDescriptor = openNullDevice();
    if (nullDescriptor < 0 || !createCloexecPipe(errorPipe) || !createCloexecPipe(resultPipe)) {
        closeDescriptor(nullDescriptor);
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        closeDescriptor(resultPipe[0]);
        closeDescriptor(resultPipe[1]);
        return failure(ErrorCode::io_error,
                       "The detached application handshake could not be created.",
                       "Retry the launch and review process descriptor limits.");
    }

    if (cancellation.isCancellationRequested()) {
        closeDescriptor(nullDescriptor);
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        closeDescriptor(resultPipe[0]);
        closeDescriptor(resultPipe[1]);
        return failure(ErrorCode::cancelled, "The detached application launch was cancelled.",
                       "Discard the stale request and begin a current launch if needed.");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + maximumDetachedApplicationHandshakeDuration;
    const pid_t broker = ::fork();
    if (broker < 0) {
        closeDescriptor(nullDescriptor);
        closeDescriptor(errorPipe[0]);
        closeDescriptor(errorPipe[1]);
        closeDescriptor(resultPipe[0]);
        closeDescriptor(resultPipe[1]);
        return failure(ErrorCode::io_error, "The detached launch broker could not be created.",
                       "Retry the launch and review process resource limits.");
    }
    if (broker == 0) {
        runBroker(prepared.value(), nullDescriptor, errorPipe, resultPipe);
    }

    closeDescriptor(nullDescriptor);
    closeDescriptor(errorPipe[1]);
    closeDescriptor(resultPipe[1]);

    auto brokerRecord =
        readWireRecord(resultPipe[0], deadline,
                       "The detached launch broker handshake exceeded its bounded deadline.",
                       "The detached launch broker handshake failed.");
    closeDescriptor(resultPipe[0]);
    if (!brokerRecord) {
        closeDescriptor(errorPipe[0]);
        killGroupAndReapBroker(broker);
        return Result<void>::failure(brokerRecord.error());
    }
    if (brokerRecord.value().stage != static_cast<std::uint32_t>(LaunchStage::brokerReady) ||
        brokerRecord.value().errorNumber != 0) {
        closeDescriptor(errorPipe[0]);
        auto brokerWait = reapBroker(broker, deadline);
        if (!brokerWait) {
            killGroupAndReapBroker(broker);
            return brokerWait;
        }
        return Result<void>::failure(errorForRecord(brokerRecord.value()));
    }

    auto execOutcome = readExecOutcome(errorPipe[0], deadline);
    closeDescriptor(errorPipe[0]);
    if (!execOutcome) {
        killGroupAndReapBroker(broker);
        return Result<void>::failure(execOutcome.error());
    }

    auto brokerWait = reapBroker(broker, deadline);
    if (!brokerWait) {
        killGroupAndReapBroker(broker);
        return brokerWait;
    }
    if (execOutcome.value()) {
        return Result<void>::failure(errorForRecord(*execOutcome.value()));
    }
    return Result<void>::success();
}

} // namespace prismdrake::launcher
