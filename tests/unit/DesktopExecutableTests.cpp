#include "DesktopExecutable.hpp"
#include "Result.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
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

class ExecutableTree final {
  public:
    ExecutableTree() {
        std::string pattern = "/tmp/prismdrake-desktop-executable-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create an executable lookup fixture."};
        }
        root_ = created;
    }

    ~ExecutableTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    ExecutableTree(const ExecutableTree &) = delete;
    ExecutableTree &operator=(const ExecutableTree &) = delete;

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

    std::filesystem::path write(std::filesystem::path relative, bool executable = true) const {
        const auto path = root_ / std::move(relative);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error{"Could not create an executable lookup fixture file."};
        }
        output << "fixture\n";
        output.close();
        if (::chmod(path.c_str(), executable ? 0700U : 0600U) != 0) {
            throw std::runtime_error{"Could not set executable lookup fixture permissions."};
        }
        return path;
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] DesktopExecutableLookupContext context(std::string searchPath,
                                                     const std::filesystem::path &lookupBase) {
    return {std::move(searchPath), lookupBase};
}

TEST(DesktopExecutableTest, ResolvesAnAbsoluteRegularExecutable) {
    ExecutableTree tree;
    const auto executable = tree.write("absolute/tool");

    const auto resolved = resolveDesktopExecutable(executable.string(), context("", tree.root()));

    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved.value().path(), executable.lexically_normal());
    EXPECT_TRUE(resolved.value().path().is_absolute());
}

TEST(DesktopExecutableTest, AbsoluteLookupStillValidatesTheCompletePathEnvelope) {
    ExecutableTree tree;
    const auto executable = tree.write("absolute/tool");
    const auto oversized = resolveDesktopExecutable(
        executable.string(),
        context(std::string(maximumDesktopExecutableSearchPathBytes + 1U, 'p'), tree.root()));
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);

    std::string nulPath{"/bin\0/private", 13U};
    const auto nul = resolveDesktopExecutable(executable.string(), context(nulPath, tree.root()));
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);

    const std::string tooManyComponents(maximumDesktopExecutableSearchPathComponents, ':');
    const auto components =
        resolveDesktopExecutable(executable.string(), context(tooManyComponents, tree.root()));
    ASSERT_FALSE(components);
    EXPECT_EQ(components.error().code, ErrorCode::too_large);
}

TEST(DesktopExecutableTest, UsesFirstExecutableInPathOrder) {
    ExecutableTree tree;
    const auto first = tree.write("first/tool");
    tree.write("second/tool");

    const auto resolved = resolveDesktopExecutable(
        "tool", context((tree.root() / "first").string() + ":" + (tree.root() / "second").string(),
                        tree.root()));

    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved.value().path(), first);
}

TEST(DesktopExecutableTest, SkipsEarlierMissingNonRegularAndNonExecutableCandidates) {
    ExecutableTree tree;
    std::filesystem::create_directories(tree.root() / "directory" / "tool");
    tree.write("disabled/tool", false);
    const auto executable = tree.write("available/tool");
    const std::string path =
        (tree.root() / "missing").string() + ":" + (tree.root() / "directory").string() + ":" +
        (tree.root() / "disabled").string() + ":" + (tree.root() / "available").string();

    const auto resolved = resolveDesktopExecutable("tool", context(path, tree.root()));

    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved.value().path(), executable);
}

TEST(DesktopExecutableTest, ResolvesEmptyAndRelativePathComponentsAgainstExplicitBase) {
    ExecutableTree tree;
    const auto baseTool = tree.write("base-tool");
    const auto relativeTool = tree.write("relative/bin/relative-tool");

    const auto empty = resolveDesktopExecutable("base-tool", context("", tree.root()));
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty.value().path(), baseTool);

    const auto leadingEmpty = resolveDesktopExecutable(
        "base-tool", context(":" + (tree.root() / "unused").string(), tree.root()));
    ASSERT_TRUE(leadingEmpty);
    EXPECT_EQ(leadingEmpty.value().path(), baseTool);

    const auto trailingEmpty = resolveDesktopExecutable(
        "base-tool", context((tree.root() / "unused").string() + ":", tree.root()));
    ASSERT_TRUE(trailingEmpty);
    EXPECT_EQ(trailingEmpty.value().path(), baseTool);

    const auto relative =
        resolveDesktopExecutable("relative-tool", context("relative/bin", tree.root()));
    ASSERT_TRUE(relative);
    EXPECT_EQ(relative.value().path(), relativeTool);
}

TEST(DesktopExecutableTest, FollowsARegularExecutableSymlinkWithoutCanonicalizingOutput) {
    ExecutableTree tree;
    const auto target = tree.write("targets/actual");
    const auto directory = tree.root() / "links";
    std::filesystem::create_directories(directory);
    const auto link = directory / "tool";
    std::filesystem::create_symlink(target, link);

    const auto resolved =
        resolveDesktopExecutable("tool", context(directory.string(), tree.root()));

    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved.value().path(), link);
    EXPECT_TRUE(std::filesystem::is_symlink(link));
}

TEST(DesktopExecutableTest, ReportsMissingDirectoryAndNonExecutableActualExec) {
    ExecutableTree tree;
    const auto nonExecutable = tree.write("nonexec", false);
    const auto directory = tree.root() / "directory";
    std::filesystem::create_directory(directory);

    const auto missing =
        resolveDesktopExecutable((tree.root() / "missing").string(), context("", tree.root()));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::not_found);

    const auto nonRegular = resolveDesktopExecutable(directory.string(), context("", tree.root()));
    ASSERT_FALSE(nonRegular);
    EXPECT_EQ(nonRegular.error().code, ErrorCode::unsupported);

    const auto denied = resolveDesktopExecutable(nonExecutable.string(), context("", tree.root()));
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, ErrorCode::permission_denied);
}

TEST(DesktopExecutableTest, ReportsFirstNonMissingPathOutcomeAfterExhaustion) {
    ExecutableTree tree;
    std::filesystem::create_directories(tree.root() / "directory" / "tool");
    tree.write("disabled/tool", false);
    const std::string directoryFirst =
        (tree.root() / "directory").string() + ":" + (tree.root() / "disabled").string();
    const std::string disabledFirst =
        (tree.root() / "disabled").string() + ":" + (tree.root() / "directory").string();

    const auto nonRegular = resolveDesktopExecutable("tool", context(directoryFirst, tree.root()));
    ASSERT_FALSE(nonRegular);
    EXPECT_EQ(nonRegular.error().code, ErrorCode::unsupported);

    const auto denied = resolveDesktopExecutable("tool", context(disabledFirst, tree.root()));
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, ErrorCode::permission_denied);
}

TEST(DesktopExecutableTest, TryExecAbsenceIsEligibleWithAValidExplicitContext) {
    ExecutableTree tree;

    const auto eligibility = evaluateDesktopTryExec(std::nullopt, context("", tree.root()));

    ASSERT_TRUE(eligibility);
    EXPECT_EQ(eligibility.value().reason, DesktopTryExecEligibilityReason::eligibleWithoutTryExec);
    EXPECT_TRUE(isEligible(eligibility.value().reason));
    EXPECT_FALSE(eligibility.value().resolvedPath);
}

TEST(DesktopExecutableTest, TryExecAbsenceStillValidatesTheCompleteContextEnvelope) {
    ExecutableTree tree;
    auto malformed =
        context(std::string(maximumDesktopExecutableSearchPathBytes + 1U, 'x'), tree.root());

    const auto eligibility = evaluateDesktopTryExec(std::nullopt, malformed);

    ASSERT_FALSE(eligibility);
    EXPECT_EQ(eligibility.error().code, ErrorCode::too_large);
}

TEST(DesktopExecutableTest, TryExecReturnsClosedAvailableAndUnavailableReasons) {
    ExecutableTree tree;
    const auto executable = tree.write("bin/available");
    tree.write("bin/disabled", false);
    std::filesystem::create_directory(tree.root() / "bin" / "directory");
    const auto lookup = context((tree.root() / "bin").string(), tree.root());

    const auto available = evaluateDesktopTryExec(std::string{"available"}, lookup);
    ASSERT_TRUE(available);
    EXPECT_EQ(available.value().reason, DesktopTryExecEligibilityReason::eligibleExecutable);
    EXPECT_TRUE(isEligible(available.value().reason));
    EXPECT_EQ(available.value().resolvedPath, executable);

    const auto missing = evaluateDesktopTryExec(std::string{"missing"}, lookup);
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing.value().reason, DesktopTryExecEligibilityReason::ineligibleMissing);
    EXPECT_FALSE(isEligible(missing.value().reason));
    EXPECT_FALSE(missing.value().resolvedPath);

    const auto directory = evaluateDesktopTryExec(std::string{"directory"}, lookup);
    ASSERT_TRUE(directory);
    EXPECT_EQ(directory.value().reason, DesktopTryExecEligibilityReason::ineligibleNotRegularFile);
    EXPECT_FALSE(directory.value().resolvedPath);

    const auto disabled = evaluateDesktopTryExec(std::string{"disabled"}, lookup);
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled.value().reason, DesktopTryExecEligibilityReason::ineligibleNotExecutable);
    EXPECT_FALSE(disabled.value().resolvedPath);
}

TEST(DesktopExecutableTest, TryExecNeverSubstitutesForActualExecResolution) {
    ExecutableTree tree;
    tree.write("bin/guard");
    const auto lookup = context((tree.root() / "bin").string(), tree.root());
    const auto eligibility = evaluateDesktopTryExec(std::string{"guard"}, lookup);
    ASSERT_TRUE(eligibility);
    ASSERT_TRUE(isEligible(eligibility.value().reason));

    const auto actual = resolveDesktopExecutable("missing-application", lookup);

    ASSERT_FALSE(actual);
    EXPECT_EQ(actual.error().code, ErrorCode::not_found);
}

TEST(DesktopExecutableTest, RejectsMalformedExecutableNamesAndPaths) {
    ExecutableTree tree;
    const auto lookup = context("", tree.root());
    std::string nul{"private\0tool", 12U};
    std::string nonAscii{"tool-\xC3\xA9", 7U};
    const std::vector<std::string> invalid{"", "relative/tool", "bad=name", nul, nonAscii};
    for (const auto &value : invalid) {
        const auto result = resolveDesktopExecutable(value, lookup);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    }

    const auto oversized =
        resolveDesktopExecutable(std::string(maximumDesktopExecutableNameBytes + 1U, 'a'), lookup);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopExecutableTest, AcceptsExactBareNameBoundary) {
    ExecutableTree tree;
    const std::string name(maximumDesktopExecutableNameBytes, 'a');
    const auto executable = tree.write(name);

    const auto result = resolveDesktopExecutable(name, context("", tree.root()));

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().path(), executable);
}

TEST(DesktopExecutableTest, RejectsMalformedLookupBase) {
    ExecutableTree tree;
    const auto relative = resolveDesktopExecutable("tool", context("", "relative/base"));
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code, ErrorCode::invalid_argument);

    std::string nulBase{"/tmp/private\0base", 17U};
    const auto nul = resolveDesktopExecutable("tool", context("", nulBase));
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);

    const std::string oversizedBase =
        "/" + std::string(maximumDesktopExecutableCandidateBytes, 'a');
    const auto oversized = resolveDesktopExecutable("tool", context("", oversizedBase));
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::invalid_argument);
}

TEST(DesktopExecutableTest, EnforcesSearchPathByteAndComponentBounds) {
    ExecutableTree tree;
    const auto tooManyBytes = resolveDesktopExecutable(
        "tool",
        context(std::string(maximumDesktopExecutableSearchPathBytes + 1U, 'a'), tree.root()));
    ASSERT_FALSE(tooManyBytes);
    EXPECT_EQ(tooManyBytes.error().code, ErrorCode::too_large);

    const std::string exactComponents(maximumDesktopExecutableSearchPathComponents - 1U, ':');
    const auto tool = tree.write("tool");
    const auto exact = resolveDesktopExecutable("tool", context(exactComponents, tree.root()));
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().path(), tool);

    const std::string tooManyComponents(maximumDesktopExecutableSearchPathComponents, ':');
    const auto tooMany = resolveDesktopExecutable("tool", context(tooManyComponents, tree.root()));
    ASSERT_FALSE(tooMany);
    EXPECT_EQ(tooMany.error().code, ErrorCode::too_large);

    std::string nulPath{"/bin\0/private", 13U};
    const auto nul = resolveDesktopExecutable("tool", context(nulPath, tree.root()));
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);
}

TEST(DesktopExecutableTest, EnforcesCandidatePathBound) {
    ExecutableTree tree;
    const auto baseBytes = tree.root().native().size();
    ASSERT_LT(baseBytes + 2U, maximumDesktopExecutableCandidateBytes);
    const std::string relativeComponent(maximumDesktopExecutableCandidateBytes - baseBytes + 1U,
                                        'a');

    const auto result = resolveDesktopExecutable("tool", context(relativeComponent, tree.root()));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(DesktopExecutableTest, MalformedTryExecIsABoundedFailure) {
    ExecutableTree tree;
    const auto malformed =
        evaluateDesktopTryExec(std::string{"relative/private-tool"}, context("", tree.root()));

    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(malformed.error().message.find("private-tool"), std::string::npos);
    EXPECT_EQ(malformed.error().recovery.find("private-tool"), std::string::npos);
}

TEST(DesktopExecutableTest, ErrorsNeverDiscloseExecutablePathOrSearchPath) {
    ExecutableTree tree;
    const auto executable =
        resolveDesktopExecutable("bad=private-executable-sentinel", context("", tree.root()));
    ASSERT_FALSE(executable);
    EXPECT_EQ(executable.error().message.find("private-executable-sentinel"), std::string::npos);
    EXPECT_EQ(executable.error().recovery.find("private-executable-sentinel"), std::string::npos);

    const auto searchPath = resolveDesktopExecutable(
        "tool", context(std::string(maximumDesktopExecutableSearchPathBytes + 1U, 'p') +
                            "private-path-sentinel",
                        tree.root()));
    ASSERT_FALSE(searchPath);
    EXPECT_EQ(searchPath.error().message.find("private-path-sentinel"), std::string::npos);
    EXPECT_EQ(searchPath.error().recovery.find("private-path-sentinel"), std::string::npos);
}

} // namespace
} // namespace prismdrake::launcher
