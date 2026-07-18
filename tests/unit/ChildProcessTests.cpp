#include "ChildProcess.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace prismdrake::session {
namespace {

using namespace std::chrono_literals;
using foundation::ErrorCode;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::array<char, 56U> pathTemplate{};
        constexpr std::string_view pattern = "/tmp/prismdrake-child-process-test.XXXXXX";
        std::copy(pattern.begin(), pattern.end(), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class ScopedEnvironmentValue final {
  public:
    ScopedEnvironmentValue(const char *name, const char *value) : name_(name) {
        if (const char *previous = std::getenv(name); previous != nullptr) {
            previous_ = previous;
        }
        if (::setenv(name, value, 1) != 0) {
            throw std::runtime_error("setenv failed");
        }
    }

    ~ScopedEnvironmentValue() {
        if (previous_) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentValue(const ScopedEnvironmentValue &) = delete;
    ScopedEnvironmentValue &operator=(const ScopedEnvironmentValue &) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

class ChildCleanup final {
  public:
    explicit ChildCleanup(ChildProcess &child) : child_(child) {}
    ~ChildCleanup() {
        if (child_.waitable()) {
            (void)child_.sendKill();
            (void)child_.waitBlocking();
        }
    }

    ChildCleanup(const ChildCleanup &) = delete;
    ChildCleanup &operator=(const ChildCleanup &) = delete;

  private:
    ChildProcess &child_;
};

[[nodiscard]] std::filesystem::path fixtureExecutable() {
    const char *value = std::getenv("PRISMDRAKE_SESSION_CHILD_FIXTURE");
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("PRISMDRAKE_SESSION_CHILD_FIXTURE is not set");
    }
    return value;
}

[[nodiscard]] PreparedSessionEnvironment childEnvironment() {
    return PreparedSessionEnvironment{
        {"DISPLAY=:77", "XDG_RUNTIME_DIR=/tmp", "DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/test-bus",
         "ONLY_PREPARED=exact-value", "XDG_CURRENT_DESKTOP=Prismdrake",
         "XDG_SESSION_DESKTOP=prismdrake", "DESKTOP_SESSION=prismdrake",
         "ASAN_OPTIONS=detect_leaks=0"},
        ":77",
        "/tmp",
        "unix:path=/tmp/test-bus"};
}

[[nodiscard]] ChildProcess launchFixture(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "prismdrake-session-child-fixture");
    auto launched = launchChildProcess(ChildLaunch{fixtureExecutable(), std::move(arguments)},
                                       childEnvironment());
    if (!launched) {
        throw std::runtime_error(launched.error().message);
    }
    return std::move(launched).value();
}

[[nodiscard]] bool waitForFile(const std::filesystem::path &path) {
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

TEST(ChildProcessTest, PassesOnlyExplicitArgvAndPreparedEnvironmentWithoutAShell) {
    ScopedEnvironmentValue parentOnly{"MUST_NOT_INHERIT", "private-parent-value"};
    auto child = launchFixture(
        {"--verify", "ONLY_PREPARED", "exact-value", "MUST_NOT_INHERIT", "literal;$(false)"});
    ChildCleanup cleanup{child};

    const auto status = child.waitBlocking();
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value(), (ChildExitStatus{ChildExitKind::exited, 0, false}));
    EXPECT_FALSE(child.waitable());
}

TEST(ChildProcessTest, ReportsExitAndSupportsPidScopedNonBlockingWait) {
    auto child = launchFixture({"--wait"});
    ChildCleanup cleanup{child};
    ASSERT_GT(child.pid(), 0);
    const auto running = child.waitNonBlocking();
    ASSERT_TRUE(running);
    EXPECT_FALSE(running.value());

    ASSERT_TRUE(child.sendTerminate());
    const auto status = child.waitBlocking();
    ASSERT_TRUE(status);
    EXPECT_EQ(status.value().kind, ChildExitKind::signaled);
    EXPECT_EQ(status.value().value, SIGTERM);
    EXPECT_FALSE(child.waitable());
    EXPECT_FALSE(child.sendKill());
}

TEST(ChildProcessTest, SendsKillOnlyToTheSelectedChildPid) {
    TemporaryDirectory temporary;
    const auto firstReady = temporary.path() / "first.ready";
    const auto secondReady = temporary.path() / "second.ready";
    auto first = launchFixture({"--ignore-term", firstReady.string()});
    ChildCleanup firstCleanup{first};
    auto second = launchFixture({"--ignore-term", secondReady.string()});
    ChildCleanup secondCleanup{second};
    ASSERT_TRUE(waitForFile(firstReady));
    ASSERT_TRUE(waitForFile(secondReady));

    ASSERT_TRUE(first.sendTerminate());
    const auto firstStillRunning = first.waitNonBlocking();
    ASSERT_TRUE(firstStillRunning);
    EXPECT_FALSE(firstStillRunning.value());
    ASSERT_TRUE(first.sendKill());
    const auto firstStatus = first.waitBlocking();
    ASSERT_TRUE(firstStatus);
    EXPECT_EQ(firstStatus.value().value, SIGKILL);

    const auto secondStillRunning = second.waitNonBlocking();
    ASSERT_TRUE(secondStillRunning);
    EXPECT_FALSE(secondStillRunning.value());
    ASSERT_TRUE(second.sendKill());
    ASSERT_TRUE(second.waitBlocking());
}

volatile std::sig_atomic_t interruptionObserved = 0;

extern "C" void observeInterruption(int) { interruptionObserved = 1; }

TEST(ChildProcessTest, RetriesBlockingWaitAfterEintr) {
    struct sigaction action = {};
    action.sa_handler = observeInterruption;
    ASSERT_EQ(::sigemptyset(&action.sa_mask), 0);
    struct sigaction previous = {};
    ASSERT_EQ(::sigaction(SIGUSR1, &action, &previous), 0);
    interruptionObserved = 0;

    auto child = launchFixture({"--sleep-exit", "100"});
    ChildCleanup cleanup{child};
    std::thread interrupter([] {
        std::this_thread::sleep_for(10ms);
        (void)::kill(::getpid(), SIGUSR1);
    });
    const auto status = child.waitBlocking();
    interrupter.join();
    ASSERT_EQ(::sigaction(SIGUSR1, &previous, nullptr), 0);

    ASSERT_TRUE(status);
    EXPECT_EQ(interruptionObserved, 1);
    EXPECT_EQ(status.value(), (ChildExitStatus{ChildExitKind::exited, 17, false}));
}

extern "C" void stopForkedChildBeforeExec() { (void)::raise(SIGSTOP); }

TEST(ChildProcessTest, BoundsAChildStoppedBeforeExecAndReapsIt) {
    const pid_t verifier = ::fork();
    ASSERT_GE(verifier, 0);
    if (verifier == 0) {
        if (::pthread_atfork(nullptr, nullptr, stopForkedChildBeforeExec) != 0) {
            ::_exit(120);
        }
        const auto result = launchChildProcess(
            ChildLaunch{fixtureExecutable(), {"fixture", "--exit", "0"}}, childEnvironment());
        if (result || result.error().code != ErrorCode::io_error ||
            result.error().message.find("bounded deadline") == std::string::npos) {
            ::_exit(119);
        }
        int unexpectedStatus = 0;
        if (::waitpid(-1, &unexpectedStatus, WNOHANG) != -1 || errno != ECHILD) {
            ::_exit(118);
        }
        ::_exit(0);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(verifier, &status, 0);
    } while (waited < 0 && errno == EINTR);
    ASSERT_EQ(waited, verifier);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ChildProcessTest, UsesExecHandshakeAndRedactsLaunchValuesFromErrors) {
    constexpr std::string_view privatePath = "/private/sentinel/missing-executable";
    constexpr std::string_view privateArgument = "private-argument-sentinel";
    const ChildLaunch launch{{privatePath}, {"fixture", std::string{privateArgument}}};

    const auto result = launchChildProcess(launch, childEnvironment());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::not_found);
    EXPECT_EQ(result.error().message.find(privatePath), std::string::npos);
    EXPECT_EQ(result.error().recovery.find(privatePath), std::string::npos);
    EXPECT_EQ(result.error().message.find(privateArgument), std::string::npos);
    EXPECT_EQ(result.error().recovery.find(privateArgument), std::string::npos);
}

TEST(ChildProcessTest, RejectsMalformedAndOversizedLaunchesBeforeFork) {
    const auto fixture = fixtureExecutable();
    const auto emptyArgv = launchChildProcess(ChildLaunch{fixture, {}}, childEnvironment());
    ASSERT_FALSE(emptyArgv);
    EXPECT_EQ(emptyArgv.error().code, ErrorCode::invalid_argument);

    const auto relative =
        launchChildProcess(ChildLaunch{"relative", {"fixture"}}, childEnvironment());
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code, ErrorCode::invalid_argument);

    std::string oversized(maximumChildArgumentBytes + 1U, 'x');
    const auto tooLarge = launchChildProcess(
        ChildLaunch{fixture, {"fixture", std::move(oversized)}}, childEnvironment());
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, ErrorCode::too_large);

    auto malformedEnvironment = childEnvironment();
    malformedEnvironment.entries.push_back("PRIVATE_SENTINEL_WITHOUT_EQUALS");
    const auto malformed =
        launchChildProcess(ChildLaunch{fixture, {"fixture", "--exit", "0"}}, malformedEnvironment);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().message.find("SENTINEL"), std::string::npos);

    const int unboundedDescriptor = ::eventfd(0U, EFD_NONBLOCK);
    ASSERT_GE(unboundedDescriptor, 0);
    ChildLaunch unboundedLaunch{fixture, {"fixture", "--exit", "0"}};
    unboundedLaunch.inheritedDescriptor = unboundedDescriptor;
    const auto unbounded = launchChildProcess(unboundedLaunch, childEnvironment());
    static_cast<void>(::close(unboundedDescriptor));
    ASSERT_FALSE(unbounded);
    EXPECT_EQ(unbounded.error().code, ErrorCode::invalid_argument);
}

} // namespace
} // namespace prismdrake::session
