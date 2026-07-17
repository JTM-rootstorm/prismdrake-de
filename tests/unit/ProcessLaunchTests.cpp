#include "ProcessLaunch.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

class LaunchExecutableTree final {
  public:
    LaunchExecutableTree() {
        std::string pattern = "/tmp/prismdrake-process-launch-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create a process launch fixture."};
        }
        root_ = created;
    }

    ~LaunchExecutableTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    LaunchExecutableTree(const LaunchExecutableTree &) = delete;
    LaunchExecutableTree &operator=(const LaunchExecutableTree &) = delete;

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

    std::filesystem::path executable(std::filesystem::path relative) const {
        const auto path = root_ / std::move(relative);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error{"Could not create a process launch executable fixture."};
        }
        output << "fixture\n";
        output.close();
        if (::chmod(path.c_str(), 0700U) != 0) {
            throw std::runtime_error{"Could not make a process launch fixture executable."};
        }
        return path;
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] ProcessLaunchContext context(const LaunchExecutableTree &tree) {
    return {{(tree.root() / "bin").string(), tree.root()},
            tree.root() / "default-work",
            {"DISPLAY=:77", "PWD=/old/value", "LANG=C"},
            std::nullopt};
}

[[nodiscard]] DesktopEntry entry(std::optional<std::string> path = std::nullopt,
                                 bool terminal = false) {
    DesktopEntry desktopEntry;
    desktopEntry.path = std::move(path);
    desktopEntry.terminal = terminal;
    return desktopEntry;
}

[[nodiscard]] DesktopExecInvocation invocation(std::vector<std::string> argv) {
    return {std::move(argv)};
}

void expectRedacted(const foundation::Error &error, std::string_view sentinel) {
    EXPECT_EQ(error.message.find(sentinel), std::string::npos);
    EXPECT_EQ(error.recovery.find(sentinel), std::string::npos);
}

TEST(ProcessLaunchTest, BuildsDirectPlanFromOnlyExplicitInputs) {
    LaunchExecutableTree tree;
    const auto application = tree.executable("bin/tool");
    auto launchContext = context(tree);
    launchContext.environment = {"DISPLAY=:77", "PWD=/first/private", "LITERAL=$(false);value",
                                 "PWD=/second/private"};

    const auto result = makeProcessLaunchPlan(
        entry(), invocation({"tool", "literal;$(false)", "argument with spaces"}), launchContext);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().executable, application);
    EXPECT_EQ(result.value().argv,
              (std::vector<std::string>{application.string(), "literal;$(false)",
                                        "argument with spaces"}));
    EXPECT_EQ(result.value().workingDirectory, tree.root() / "default-work");
    EXPECT_EQ(
        result.value().environment,
        (std::vector<std::string>{"DISPLAY=:77", "PWD=" + (tree.root() / "default-work").string(),
                                  "LITERAL=$(false);value"}));
}

TEST(ProcessLaunchTest, AppendsPwdWhenTheExplicitEnvironmentHasNone) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    launchContext.environment = {"DISPLAY=:77", "LANG=C"};

    const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().environment,
              (std::vector<std::string>{"DISPLAY=:77", "LANG=C",
                                        "PWD=" + (tree.root() / "default-work").string()}));
}

TEST(ProcessLaunchTest, ResolvesRelativeAbsoluteAndEmptyDesktopPathsLexically) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    const auto launchContext = context(tree);

    const auto relative = makeProcessLaunchPlan(entry(std::string{"one/../selected"}),
                                                invocation({"tool"}), launchContext);
    ASSERT_TRUE(relative);
    EXPECT_EQ(relative.value().workingDirectory,
              (tree.root() / "default-work" / "selected").lexically_normal());

    const auto absolutePath = tree.root() / "absolute" / ".." / "chosen";
    const auto absolute =
        makeProcessLaunchPlan(entry(absolutePath.string()), invocation({"tool"}), launchContext);
    ASSERT_TRUE(absolute);
    EXPECT_EQ(absolute.value().workingDirectory, absolutePath.lexically_normal());

    const auto empty =
        makeProcessLaunchPlan(entry(std::string{}), invocation({"tool"}), launchContext);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.value().workingDirectory, tree.root() / "default-work");
}

TEST(ProcessLaunchTest, DoesNotRequireTheSelectedWorkingDirectoryToExistDuringPlanning) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    launchContext.defaultWorkingDirectory = tree.root() / "missing" / "default";

    const auto result = makeProcessLaunchPlan(entry(std::string{"also/missing"}),
                                              invocation({"tool"}), launchContext);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().workingDirectory,
              tree.root() / "missing" / "default" / "also" / "missing");
    EXPECT_FALSE(std::filesystem::exists(result.value().workingDirectory));
}

TEST(ProcessLaunchTest, WrapsTerminalInvocationAsOneExactArgumentVector) {
    LaunchExecutableTree tree;
    const auto application = tree.executable("bin/application");
    const auto terminal = tree.executable("bin/terminal");
    auto launchContext = context(tree);
    launchContext.terminal = TerminalLaunchPolicy{"terminal", {"--execute", "literal;$(false)"}};

    const auto result =
        makeProcessLaunchPlan(entry(std::nullopt, true),
                              invocation({"application", "argument with spaces"}), launchContext);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().executable, terminal);
    EXPECT_EQ(result.value().argv,
              (std::vector<std::string>{terminal.string(), "--execute", "literal;$(false)",
                                        application.string(), "argument with spaces"}));
}

TEST(ProcessLaunchTest, RejectsTerminalRequirementBeforeAnyExecutableLookupWhenUnavailable) {
    LaunchExecutableTree tree;
    auto launchContext = context(tree);

    const auto result = makeProcessLaunchPlan(
        entry(std::nullopt, true), invocation({"private-missing-application"}), launchContext);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
    expectRedacted(result.error(), "private-missing-application");
}

TEST(ProcessLaunchTest, IgnoresAnUnusedTerminalPolicyForDirectLaunch) {
    LaunchExecutableTree tree;
    const auto application = tree.executable("bin/application");
    auto launchContext = context(tree);
    launchContext.terminal = TerminalLaunchPolicy{"missing-terminal", {"--execute"}};

    const auto result = makeProcessLaunchPlan(entry(), invocation({"application"}), launchContext);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().executable, application);
    EXPECT_EQ(result.value().argv, (std::vector<std::string>{application.string()}));
}

TEST(ProcessLaunchTest, RejectsDbusActivationAtTheProcessPlanningBoundary) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto desktopEntry = entry();
    desktopEntry.dbusActivatable = true;

    const auto result = makeProcessLaunchPlan(desktopEntry, invocation({"tool"}), context(tree));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
}

TEST(ProcessLaunchTest, ReportsMissingApplicationAndTerminalExecutables) {
    LaunchExecutableTree tree;
    auto launchContext = context(tree);
    const auto missingApplication =
        makeProcessLaunchPlan(entry(), invocation({"missing-application"}), launchContext);
    ASSERT_FALSE(missingApplication);
    EXPECT_EQ(missingApplication.error().code, ErrorCode::not_found);

    tree.executable("bin/application");
    launchContext.terminal = TerminalLaunchPolicy{"missing-terminal", {"--execute"}};
    const auto missingTerminal = makeProcessLaunchPlan(entry(std::nullopt, true),
                                                       invocation({"application"}), launchContext);
    ASSERT_FALSE(missingTerminal);
    EXPECT_EQ(missingTerminal.error().code, ErrorCode::not_found);
}

TEST(ProcessLaunchTest, RejectsMalformedAndOversizedWorkingDirectories) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");

    auto relativeDefault = context(tree);
    relativeDefault.defaultWorkingDirectory = "relative/default";
    const auto relative = makeProcessLaunchPlan(entry(), invocation({"tool"}), relativeDefault);
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code, ErrorCode::invalid_argument);

    auto nulDefault = context(tree);
    nulDefault.defaultWorkingDirectory = std::string{"/private/default\0tail", 21U};
    const auto nul = makeProcessLaunchPlan(entry(), invocation({"tool"}), nulDefault);
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);
    expectRedacted(nul.error(), "private");

    auto oversizedDefault = context(tree);
    oversizedDefault.defaultWorkingDirectory =
        "/" + std::string(maximumProcessLaunchWorkingDirectoryBytes, 'd');
    const auto oversized = makeProcessLaunchPlan(entry(), invocation({"tool"}), oversizedDefault);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);

    std::string nulPath{"private-path\0tail", 17U};
    const auto entryNul =
        makeProcessLaunchPlan(entry(nulPath), invocation({"tool"}), context(tree));
    ASSERT_FALSE(entryNul);
    EXPECT_EQ(entryNul.error().code, ErrorCode::invalid_argument);
    expectRedacted(entryNul.error(), "private-path");

    const auto oversizedPath = makeProcessLaunchPlan(
        entry(std::string(maximumProcessLaunchWorkingDirectoryBytes + 1U, 'p')),
        invocation({"tool"}), context(tree));
    ASSERT_FALSE(oversizedPath);
    EXPECT_EQ(oversizedPath.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, RejectsMalformedAndOversizedInvocationVectors) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    const auto launchContext = context(tree);

    const auto empty = makeProcessLaunchPlan(entry(), invocation({}), launchContext);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::invalid_argument);

    const auto emptyExecutable = makeProcessLaunchPlan(entry(), invocation({""}), launchContext);
    ASSERT_FALSE(emptyExecutable);
    EXPECT_EQ(emptyExecutable.error().code, ErrorCode::invalid_argument);

    std::string nulArgument{"private-argument\0tail", 21U};
    const auto nul =
        makeProcessLaunchPlan(entry(), invocation({"tool", nulArgument}), launchContext);
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);
    expectRedacted(nul.error(), "private-argument");

    const auto oversized = makeProcessLaunchPlan(
        entry(), invocation({"tool", std::string(maximumProcessLaunchArgumentBytes + 1U, 'a')}),
        launchContext);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);

    std::vector<std::string> tooMany(maximumProcessLaunchArguments + 1U, "argument");
    tooMany.front() = "tool";
    const auto count =
        makeProcessLaunchPlan(entry(), invocation(std::move(tooMany)), launchContext);
    ASSERT_FALSE(count);
    EXPECT_EQ(count.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, RejectsMalformedAndOversizedEnvironmentSnapshots) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");

    for (const auto &malformed : {std::string{"NO_EQUALS"}, std::string{"=empty-name"},
                                  std::string{"PRIVATE=value\0tail", 18U}}) {
        auto launchContext = context(tree);
        launchContext.environment = {malformed};
        const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
        expectRedacted(result.error(), "PRIVATE");
    }

    auto oversizedEntry = context(tree);
    oversizedEntry.environment = {"VALUE=" +
                                  std::string(maximumProcessLaunchEnvironmentEntryBytes, 'v')};
    const auto entryResult = makeProcessLaunchPlan(entry(), invocation({"tool"}), oversizedEntry);
    ASSERT_FALSE(entryResult);
    EXPECT_EQ(entryResult.error().code, ErrorCode::too_large);

    auto tooMany = context(tree);
    tooMany.environment.clear();
    tooMany.environment.reserve(maximumProcessLaunchEnvironmentEntries);
    for (std::size_t index = 0U; index < maximumProcessLaunchEnvironmentEntries; ++index) {
        tooMany.environment.push_back("VALUE" + std::to_string(index) + "=x");
    }
    const auto count = makeProcessLaunchPlan(entry(), invocation({"tool"}), tooMany);
    ASSERT_FALSE(count);
    EXPECT_EQ(count.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, RejectsDuplicateNonPwdEnvironmentNamesWithoutDisclosure) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    launchContext.environment = {"private-duplicate-sentinel=first",
                                 "private-duplicate-sentinel=second"};

    const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    expectRedacted(result.error(), "private-duplicate-sentinel");
}

TEST(ProcessLaunchTest, AcceptsExactAggregateEnvironmentBoundAfterPwdCollapse) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    constexpr std::size_t entryCount = 32U;
    static_assert(maximumProcessLaunchEnvironmentBytes % entryCount == 0U);
    constexpr std::size_t contribution = maximumProcessLaunchEnvironmentBytes / entryCount;
    static_assert(contribution > 5U);
    static_assert(contribution - 1U <= maximumProcessLaunchEnvironmentEntryBytes);
    launchContext.environment.assign(
        entryCount, "PWD=" + std::string(contribution - 1U - std::string_view{"PWD="}.size(), 'p'));

    const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().environment.size(), 1U);
    EXPECT_EQ(result.value().environment.front(), "PWD=" + (tree.root() / "default-work").string());
}

TEST(ProcessLaunchTest, RejectsAggregateEnvironmentOneByteOverBound) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    constexpr std::size_t entryCount = 32U;
    constexpr std::size_t contribution = maximumProcessLaunchEnvironmentBytes / entryCount;
    launchContext.environment.assign(
        entryCount, "PWD=" + std::string(contribution - 1U - std::string_view{"PWD="}.size(), 'p'));
    launchContext.environment.front().push_back('p');

    const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, AcceptsExactEnvironmentEntryCountWhenPwdCanBeReplaced) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    auto launchContext = context(tree);
    launchContext.environment.clear();
    launchContext.environment.reserve(maximumProcessLaunchEnvironmentEntries);
    launchContext.environment.push_back("PWD=/replace/me");
    for (std::size_t index = 1U; index < maximumProcessLaunchEnvironmentEntries; ++index) {
        launchContext.environment.push_back("VALUE" + std::to_string(index) + "=x");
    }

    const auto result = makeProcessLaunchPlan(entry(), invocation({"tool"}), launchContext);

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().environment.size(), maximumProcessLaunchEnvironmentEntries);
    EXPECT_EQ(result.value().environment.front(), "PWD=" + (tree.root() / "default-work").string());
}

TEST(ProcessLaunchTest, EnforcesTheCompleteArgumentAndEnvironmentEnvelope) {
    LaunchExecutableTree tree;
    tree.executable("bin/tool");
    std::vector<std::string> arguments(17U, std::string(maximumProcessLaunchArgumentBytes, 'a'));
    arguments.front() = "tool";

    const auto result =
        makeProcessLaunchPlan(entry(), invocation(std::move(arguments)), context(tree));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, AppliesFinalArgumentCountAfterTerminalWrapping) {
    LaunchExecutableTree tree;
    tree.executable("bin/application");
    tree.executable("bin/terminal");
    auto launchContext = context(tree);
    launchContext.terminal = TerminalLaunchPolicy{"terminal", std::vector<std::string>(6U, "p")};
    std::vector<std::string> applicationArguments(250U, "a");
    applicationArguments.front() = "application";

    const auto result = makeProcessLaunchPlan(
        entry(std::nullopt, true), invocation(std::move(applicationArguments)), launchContext);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(ProcessLaunchTest, ErrorsNeverDiscloseLaunchInputs) {
    LaunchExecutableTree tree;
    tree.executable("bin/application");

    const auto application = makeProcessLaunchPlan(
        entry(), invocation({"bad=private-application-sentinel"}), context(tree));
    ASSERT_FALSE(application);
    expectRedacted(application.error(), "private-application-sentinel");

    auto terminalContext = context(tree);
    terminalContext.terminal = TerminalLaunchPolicy{"bad=private-terminal-sentinel", {"--execute"}};
    const auto terminal = makeProcessLaunchPlan(entry(std::nullopt, true),
                                                invocation({"application"}), terminalContext);
    ASSERT_FALSE(terminal);
    expectRedacted(terminal.error(), "private-terminal-sentinel");

    auto environmentContext = context(tree);
    environmentContext.environment = {"private-environment-sentinel"};
    const auto environment =
        makeProcessLaunchPlan(entry(), invocation({"application"}), environmentContext);
    ASSERT_FALSE(environment);
    expectRedacted(environment.error(), "private-environment-sentinel");
}

} // namespace
} // namespace prismdrake::launcher
