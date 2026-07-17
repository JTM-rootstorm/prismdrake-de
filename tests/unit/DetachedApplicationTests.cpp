#include "DetachedApplication.hpp"

#include "ProcessLaunch.hpp"
#include "Result.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::CancellationSource;
using foundation::Error;
using foundation::ErrorCode;

constexpr std::uint32_t fixtureMagic = 0x50444C46U;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/prismdrake-detached-application-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create a detached application test directory."};
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

struct FixtureCapture final {
    std::vector<std::string> argv;
    std::string workingDirectory;
    std::vector<std::string> environment;
};

[[nodiscard]] std::filesystem::path fixturePath() {
    const char *value = std::getenv("PRISMDRAKE_APPLICATION_LAUNCH_FIXTURE");
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error{"PRISMDRAKE_APPLICATION_LAUNCH_FIXTURE is not set"};
    }
    return value;
}

[[nodiscard]] bool readUnsigned(std::ifstream &stream, std::uint64_t &value) {
    return static_cast<bool>(stream.read(reinterpret_cast<char *>(&value), sizeof(value)));
}

[[nodiscard]] bool readString(std::ifstream &stream, std::string &value) {
    constexpr std::uint64_t maximumFixtureStringBytes = 2U * 1024U * 1024U;
    std::uint64_t size = 0U;
    if (!readUnsigned(stream, size) || size > maximumFixtureStringBytes) {
        return false;
    }
    value.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(stream.read(value.data(), static_cast<std::streamsize>(value.size())));
}

[[nodiscard]] std::optional<FixtureCapture> readCapture(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::uint32_t magic = 0U;
    if (!input.read(reinterpret_cast<char *>(&magic), sizeof(magic)) || magic != fixtureMagic) {
        return std::nullopt;
    }

    std::uint64_t argumentCount = 0U;
    if (!readUnsigned(input, argumentCount) || argumentCount > maximumProcessLaunchArguments) {
        return std::nullopt;
    }
    FixtureCapture capture;
    capture.argv.resize(static_cast<std::size_t>(argumentCount));
    for (auto &argument : capture.argv) {
        if (!readString(input, argument)) {
            return std::nullopt;
        }
    }
    if (!readString(input, capture.workingDirectory)) {
        return std::nullopt;
    }
    std::uint64_t environmentCount = 0U;
    if (!readUnsigned(input, environmentCount) ||
        environmentCount > maximumProcessLaunchEnvironmentEntries) {
        return std::nullopt;
    }
    capture.environment.resize(static_cast<std::size_t>(environmentCount));
    for (auto &entry : capture.environment) {
        if (!readString(input, entry)) {
            return std::nullopt;
        }
    }
    return capture;
}

[[nodiscard]] FixtureCapture waitForCapture(const std::filesystem::path &path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto capture = readCapture(path)) {
            return std::move(*capture);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ADD_FAILURE() << "detached application fixture did not publish a complete capture";
    return {};
}

[[nodiscard]] ProcessLaunchPlan fixturePlan(const std::filesystem::path &workingDirectory,
                                            const std::filesystem::path &output,
                                            std::vector<std::string> trailingArguments = {},
                                            std::vector<std::string> environment = {}) {
    const auto fixture = fixturePath().lexically_normal();
    ProcessLaunchPlan plan;
    plan.executable = fixture;
    plan.argv.reserve(trailingArguments.size() + 2U);
    plan.argv.push_back(fixture.native());
    plan.argv.push_back(output.native());
    plan.argv.insert(plan.argv.end(), std::make_move_iterator(trailingArguments.begin()),
                     std::make_move_iterator(trailingArguments.end()));
    plan.workingDirectory = workingDirectory.lexically_normal();
    plan.environment = std::move(environment);
    plan.environment.push_back("PWD=" + plan.workingDirectory.native());
    return plan;
}

[[nodiscard]] foundation::Result<void> launch(const ProcessLaunchPlan &plan) {
    CancellationSource cancellation;
    return launchDetachedApplication(plan, cancellation.token());
}

void expectRedacted(const Error &error, std::string_view privateText) {
    EXPECT_EQ(error.message.find(privateText), std::string::npos);
    EXPECT_EQ(error.recovery.find(privateText), std::string::npos);
}

void expectNoWaitableChildren() {
    int status = 0;
    errno = 0;
    const auto result = ::waitpid(-1, &status, WNOHANG);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(DetachedApplicationTest, ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell) {
    TemporaryDirectory temporary;
    const auto output = temporary.path() / "capture.bin";
    const auto shellMarker = temporary.path() / "must-not-exist";
    const std::string literal = "; $(touch " + shellMarker.native() + ") & | `false` * ?";
    const auto plan = fixturePlan(temporary.path(), output, {literal, "quote\"slash\\value"},
                                  {"TOKEN=exact value", "EMPTY="});

    const auto result = launch(plan);

    ASSERT_TRUE(result) << (result ? "" : result.error().message);
    const auto captured = waitForCapture(output);
    EXPECT_EQ(captured.argv, plan.argv);
    EXPECT_EQ(captured.workingDirectory, plan.workingDirectory.native());
    EXPECT_EQ(captured.environment, plan.environment);
    EXPECT_FALSE(std::filesystem::exists(shellMarker));
    expectNoWaitableChildren();
}

TEST(DetachedApplicationTest, ReportsMissingAndUnlinkedExecutablesWithoutLeavingChildren) {
    TemporaryDirectory temporary;
    const auto missing = temporary.path() / "missing-fixture";
    auto missingPlan = fixturePlan(temporary.path(), temporary.path() / "missing.bin");
    missingPlan.executable = missing;
    missingPlan.argv.front() = missing.native();
    const auto missingResult = launch(missingPlan);
    ASSERT_FALSE(missingResult);
    EXPECT_EQ(missingResult.error().code, ErrorCode::not_found);
    expectNoWaitableChildren();

    const auto copied = temporary.path() / "unlinked-fixture";
    std::filesystem::copy_file(fixturePath(), copied);
    std::filesystem::permissions(copied, std::filesystem::perms::owner_all);
    auto unlinkedPlan = fixturePlan(temporary.path(), temporary.path() / "unlinked.bin");
    unlinkedPlan.executable = copied;
    unlinkedPlan.argv.front() = copied.native();
    std::filesystem::remove(copied);
    const auto unlinkedResult = launch(unlinkedPlan);
    ASSERT_FALSE(unlinkedResult);
    EXPECT_EQ(unlinkedResult.error().code, ErrorCode::not_found);
    expectNoWaitableChildren();
}

TEST(DetachedApplicationTest, ReportsWorkingDirectoryFailureWithoutExecutingFixture) {
    TemporaryDirectory temporary;
    const auto output = temporary.path() / "capture.bin";
    const auto unavailable = temporary.path() / "missing-directory";
    const auto plan = fixturePlan(unavailable, output);

    const auto result = launch(plan);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::not_found);
    EXPECT_FALSE(std::filesystem::exists(output));
    expectNoWaitableChildren();
}

TEST(DetachedApplicationTest, ObservesCancellationOnlyBeforeTheForkCommitPoint) {
    TemporaryDirectory temporary;
    const auto output = temporary.path() / "capture.bin";
    const auto plan = fixturePlan(temporary.path(), output);
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.requestCancellation());

    const auto result = launchDetachedApplication(plan, cancellation.token());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
    expectNoWaitableChildren();
}

TEST(DetachedApplicationTest, RevalidatesMalformedPlansWithRedactedDiagnostics) {
    TemporaryDirectory temporary;
    const auto output = temporary.path() / "capture.bin";

    auto mismatchedArgv = fixturePlan(temporary.path(), output);
    mismatchedArgv.argv.front() = "/private/argv-value";
    const auto argvResult = launch(mismatchedArgv);
    ASSERT_FALSE(argvResult);
    EXPECT_EQ(argvResult.error().code, ErrorCode::invalid_argument);
    expectRedacted(argvResult.error(), "private");

    auto invalidEnvironment = fixturePlan(temporary.path(), output);
    invalidEnvironment.environment = {"PRIVATE_NO_EQUALS", "PWD=" + temporary.path().native()};
    const auto environmentResult = launch(invalidEnvironment);
    ASSERT_FALSE(environmentResult);
    EXPECT_EQ(environmentResult.error().code, ErrorCode::invalid_environment);
    expectRedacted(environmentResult.error(), "PRIVATE");

    auto nullArgument = fixturePlan(temporary.path(), output);
    nullArgument.argv.push_back(std::string{"private\0argument", 16U});
    const auto nullResult = launch(nullArgument);
    ASSERT_FALSE(nullResult);
    EXPECT_EQ(nullResult.error().code, ErrorCode::invalid_argument);
    expectRedacted(nullResult.error(), "private");

    auto oversized = fixturePlan(temporary.path(), output);
    oversized.argv.push_back(std::string(maximumProcessLaunchArgumentBytes + 1U, 'p'));
    const auto oversizedResult = launch(oversized);
    ASSERT_FALSE(oversizedResult);
    EXPECT_EQ(oversizedResult.error().code, ErrorCode::too_large);
    expectRedacted(oversizedResult.error(), "pppp");

    auto tooManyArguments = fixturePlan(temporary.path(), output);
    tooManyArguments.argv.resize(maximumProcessLaunchArguments + 1U, "private-argument");
    tooManyArguments.argv.front() = tooManyArguments.executable.native();
    const auto argumentCountResult = launch(tooManyArguments);
    ASSERT_FALSE(argumentCountResult);
    EXPECT_EQ(argumentCountResult.error().code, ErrorCode::too_large);
    expectRedacted(argumentCountResult.error(), "private");

    auto duplicateEnvironment = fixturePlan(temporary.path(), output);
    duplicateEnvironment.environment.insert(duplicateEnvironment.environment.begin(),
                                            "PWD=/private/mismatch");
    const auto duplicateEnvironmentResult = launch(duplicateEnvironment);
    ASSERT_FALSE(duplicateEnvironmentResult);
    EXPECT_EQ(duplicateEnvironmentResult.error().code, ErrorCode::invalid_environment);
    expectRedacted(duplicateEnvironmentResult.error(), "private");

    auto relativeDirectory = fixturePlan(temporary.path(), output);
    relativeDirectory.workingDirectory = "private/relative";
    relativeDirectory.environment.back() = "PWD=private/relative";
    const auto directoryResult = launch(relativeDirectory);
    ASSERT_FALSE(directoryResult);
    EXPECT_EQ(directoryResult.error().code, ErrorCode::invalid_argument);
    expectRedacted(directoryResult.error(), "private");
    expectNoWaitableChildren();
}

TEST(DetachedApplicationTest, RepeatedLaunchesReapEveryBroker) {
    TemporaryDirectory temporary;
    constexpr std::size_t launchCount = 16U;
    for (std::size_t index = 0U; index < launchCount; ++index) {
        const auto output = temporary.path() / ("capture-" + std::to_string(index) + ".bin");
        const auto plan = fixturePlan(temporary.path(), output, {std::to_string(index)});
        const auto result = launch(plan);
        ASSERT_TRUE(result) << (result ? "" : result.error().message);
        const auto captured = waitForCapture(output);
        ASSERT_EQ(captured.argv, plan.argv);
        expectNoWaitableChildren();
    }
}

#if defined(PRISMDRAKE_DETACHED_APPLICATION_TEST_HOOK)
volatile std::sig_atomic_t delayBeforeExec = 0;

extern "C" void prismdrakeDetachedApplicationPreExecTestHook() noexcept {
    if (delayBeforeExec == 0) {
        return;
    }
    timespec delay{5, 0};
    timespec remaining{};
    while (::nanosleep(&delay, &remaining) < 0 && errno == EINTR) {
        delay = remaining;
    }
}

TEST(DetachedApplicationTest, TimesOutAndKillsTheDetachedSessionBeforeExec) {
    TemporaryDirectory temporary;
    const auto output = temporary.path() / "timeout.bin";
    const auto plan = fixturePlan(temporary.path(), output);
    delayBeforeExec = 1;
    const auto started = std::chrono::steady_clock::now();

    const auto result = launch(plan);

    const auto elapsed = std::chrono::steady_clock::now() - started;
    delayBeforeExec = 0;
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::io_error);
    EXPECT_GE(elapsed, maximumDetachedApplicationHandshakeDuration);
    EXPECT_LT(elapsed, maximumDetachedApplicationHandshakeDuration + std::chrono::seconds{1});
    EXPECT_FALSE(std::filesystem::exists(output));
    expectNoWaitableChildren();
}
#endif

} // namespace
} // namespace prismdrake::launcher
