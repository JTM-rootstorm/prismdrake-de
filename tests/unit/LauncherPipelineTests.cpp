#include "DesktopExec.hpp"
#include "DetachedApplication.hpp"
#include "ProcessLaunch.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::CancellationSource;
using foundation::ErrorCode;

constexpr std::uint32_t fixtureMagic = 0x50444C46U;

class PipelineDirectory final {
  public:
    PipelineDirectory() {
        std::string pattern = "/tmp/prismdrake-launcher-pipeline-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create a launcher pipeline test directory."};
        }
        path_ = created;
    }

    ~PipelineDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    PipelineDirectory(const PipelineDirectory &) = delete;
    PipelineDirectory &operator=(const PipelineDirectory &) = delete;

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
    return std::filesystem::path{value}.lexically_normal();
}

[[nodiscard]] std::string quoteExecArgument(const std::string &argument) {
    std::string quoted{"\""};
    for (const char character : argument) {
        if (character == '\\' || character == '"' || character == '$' || character == '`') {
            quoted.push_back('\\');
        }
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
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
    ADD_FAILURE() << "launcher pipeline fixture did not publish a complete capture";
    return {};
}

[[nodiscard]] ProcessLaunchContext launchContext(const PipelineDirectory &directory) {
    return {{directory.path().string(), directory.path()},
            directory.path(),
            {"LANG=C", "PRISMDRAKE_PIPELINE_SENTINEL=exact value"},
            std::nullopt};
}

[[nodiscard]] DesktopEntry pipelineEntry(const std::filesystem::path &output,
                                         const std::string &literalArgument) {
    DesktopEntry entry;
    entry.name = "Pipeline Fixture";
    entry.exec = quoteExecArgument(fixturePath().native()) + " " +
                 quoteExecArgument(output.native()) + " " + quoteExecArgument(literalArgument);
    return entry;
}

TEST(LauncherPipelineTest, ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell) {
    PipelineDirectory directory;
    const auto output = directory.path() / "capture.bin";
    const auto shellMarker = directory.path() / "must-not-exist";
    const std::string literal = "; $(touch " + shellMarker.native() + ") & | `false` * ?";
    const auto entry = pipelineEntry(output, literal);

    const auto invocations = expandDesktopExec(entry, {});
    ASSERT_TRUE(invocations) << (invocations ? "" : invocations.error().message);
    ASSERT_EQ(invocations.value().size(), 1U);

    const auto plan =
        makeProcessLaunchPlan(entry, invocations.value().front(), launchContext(directory));
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().message);
    CancellationSource cancellation;
    const auto launched = launchDetachedApplication(plan.value(), cancellation.token());
    ASSERT_TRUE(launched) << (launched ? "" : launched.error().message);

    const auto captured = waitForCapture(output);
    EXPECT_EQ(captured.argv, plan.value().argv);
    EXPECT_EQ(captured.workingDirectory, plan.value().workingDirectory.native());
    EXPECT_EQ(captured.environment, plan.value().environment);
    EXPECT_EQ(captured.argv.back(), literal);
    EXPECT_FALSE(std::filesystem::exists(shellMarker));
}

TEST(LauncherPipelineTest, RejectsATerminalRequirementWhenNoPolicyIsConfigured) {
    PipelineDirectory directory;
    const auto output = directory.path() / "must-not-launch.bin";
    auto entry = pipelineEntry(output, "literal argument");
    entry.terminal = true;

    const auto invocations = expandDesktopExec(entry, {});
    ASSERT_TRUE(invocations);
    ASSERT_EQ(invocations.value().size(), 1U);

    const auto plan =
        makeProcessLaunchPlan(entry, invocations.value().front(), launchContext(directory));
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().code, ErrorCode::unsupported);
    EXPECT_FALSE(std::filesystem::exists(output));
}

} // namespace
} // namespace prismdrake::launcher
