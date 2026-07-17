#include "DesktopEntryDiscovery.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::CancellationSource;
using foundation::ErrorCode;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::size_t sequence = 0U;
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-discovery-tests-" + std::to_string(::getpid()) + "-" +
                 std::to_string(sequence++));
        std::filesystem::create_directories(path_);
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

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(stream);
}

[[nodiscard]] std::string application(std::string_view name,
                                      std::string_view extra = std::string_view{}) {
    std::string document = "[Desktop Entry]\nType=Application\nName=";
    document.append(name);
    document.append("\nExec=tool\n");
    document.append(extra);
    return document;
}

[[nodiscard]] CurrentDesktopContext desktopContext(std::string_view value = "Prismdrake") {
    auto result = parseCurrentDesktopContext(value);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

[[nodiscard]] DesktopEntryDiscovery
makeDiscovery(std::vector<std::filesystem::path> roots,
              DesktopEntryDiscoveryLimits limits = DesktopEntryDiscoveryLimits{}) {
    auto result = createDesktopEntryDiscovery(ApplicationPaths{std::move(roots)}, {"C"},
                                              desktopContext(), limits);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

[[nodiscard]] DesktopEntryDiscoveryBatch runToCompletion(DesktopEntryDiscovery &discovery,
                                                         std::size_t budget = 7U) {
    CancellationSource cancellation;
    DesktopEntryDiscoveryBatch last;
    for (std::size_t pulls = 0U; pulls < 100000U; ++pulls) {
        auto result = discovery.pull(budget, cancellation.token());
        EXPECT_TRUE(result);
        if (!result) {
            return last;
        }
        last = std::move(result).value();
        if (last.snapshot->complete) {
            return last;
        }
    }
    ADD_FAILURE() << "desktop discovery did not complete within the test pull bound";
    return last;
}

[[nodiscard]] bool hasDiagnostic(const DesktopEntryDiscoverySnapshot &snapshot,
                                 DesktopEntryDiscoveryDiagnosticCode code) {
    return std::ranges::any_of(snapshot.diagnostics,
                               [code](const auto &diagnostic) { return diagnostic.code == code; });
}

TEST(DesktopEntryDiscoveryTest, UsesRootPrecedenceAndLexicalRelativePathOrder) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "z.desktop", application("High Z"));
    writeFile(high / "a.desktop", application("High A"));
    writeFile(low / "a.desktop", application("Low A"));
    writeFile(low / "b.desktop", application("Low B"));

    auto discovery = makeDiscovery({high, low});
    const auto completed = runToCompletion(discovery, 1U);

    ASSERT_TRUE(completed.snapshot->complete);
    ASSERT_EQ(completed.snapshot->entries.size(), 3U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "a.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].entry.name, "High A");
    EXPECT_EQ(completed.snapshot->entries[1].id.value(), "z.desktop");
    EXPECT_EQ(completed.snapshot->entries[2].id.value(), "b.desktop");
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 3U);
    EXPECT_EQ(completed.snapshot->visibleEntryIndices, (std::vector<std::size_t>{0U, 1U, 2U}));
}

TEST(DesktopEntryDiscoveryTest, MalformedAndHiddenHigherCandidatesShadowLowerRoots) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "broken.desktop", "private malformed desktop contents");
    writeFile(low / "broken.desktop", application("Must remain shadowed"));
    writeFile(high / "hidden.desktop", "[Desktop Entry]\nHidden=true\n");
    writeFile(low / "hidden.desktop", application("Must also remain shadowed"));

    auto discovery = makeDiscovery({high, low});
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "hidden.desktop");
    EXPECT_TRUE(completed.snapshot->entries[0].entry.hidden);
    EXPECT_EQ(completed.snapshot->entries[0].visibility,
              DesktopEntryVisibilityReason::hiddenTombstone);
    EXPECT_TRUE(completed.snapshot->visibleEntryIndices.empty());
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 2U);
    EXPECT_TRUE(hasDiagnostic(*completed.snapshot,
                              DesktopEntryDiscoveryDiagnosticCode::desktopEntryRejected));
}

TEST(DesktopEntryDiscoveryTest, FlattenedIdCollisionUsesSmallestRelativePath) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "applications";
    writeFile(root / "vendor-tool.desktop", application("Lexically first"));
    writeFile(root / "vendor" / "tool.desktop", application("Flattened collision"));

    auto discovery = makeDiscovery({root});
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "vendor-tool.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].entry.name, "Lexically first");
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 1U);
}

TEST(DesktopEntryDiscoveryTest, RetainsClosedVisibilityReasonsButPublishesOnlyVisibleIndices) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "applications";
    writeFile(root / "visible.desktop", application("Visible", "OnlyShowIn=Prismdrake;\n"));
    writeFile(root / "nodisplay.desktop", application("No display", "NoDisplay=true\n"));
    writeFile(root / "other.desktop", application("Other", "OnlyShowIn=Other;\n"));

    auto discovery = makeDiscovery({root});
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->entries.size(), 3U);
    ASSERT_EQ(completed.snapshot->visibleEntryIndices.size(), 1U);
    const auto visibleIndex = completed.snapshot->visibleEntryIndices.front();
    EXPECT_EQ(completed.snapshot->entries[visibleIndex].id.value(), "visible.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].visibility,
              DesktopEntryVisibilityReason::hiddenNoDisplay);
    EXPECT_EQ(completed.snapshot->entries[1].visibility,
              DesktopEntryVisibilityReason::hiddenWithoutOnlyShowInMatch);
    EXPECT_EQ(completed.snapshot->entries[2].visibility,
              DesktopEntryVisibilityReason::visibleForCurrentDesktop);
}

TEST(DesktopEntryDiscoveryTest,
     AcceptsRootAndRegularFileSymlinksWithoutFollowingDirectorySymlinks) {
    TemporaryDirectory temporary;
    const auto realRoot = temporary.path() / "real-root";
    const auto linkedRoot = temporary.path() / "linked-root";
    const auto external = temporary.path() / "external";
    writeFile(realRoot / "real.desktop", application("Real"));
    writeFile(external / "linked-file-target.desktop", application("Linked file"));
    writeFile(external / "nested" / "escaped.desktop", application("Must not escape"));
    std::filesystem::create_symlink(external / "linked-file-target.desktop",
                                    realRoot / "linked.desktop");
    std::filesystem::create_directory_symlink(external / "nested", realRoot / "linked-directory");
    std::filesystem::create_directory_symlink(realRoot, linkedRoot);

    auto discovery = makeDiscovery({linkedRoot});
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->entries.size(), 2U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "linked.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].location.absolutePath(),
              linkedRoot / "linked.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].location.relativePath(), "linked.desktop");
    EXPECT_EQ(completed.snapshot->entries[0].location.rootIndex(), 0U);
    EXPECT_EQ(completed.snapshot->entries[1].id.value(), "real.desktop");
    EXPECT_EQ(completed.snapshot->entries[1].location.absolutePath(), linkedRoot / "real.desktop");
    EXPECT_EQ(completed.snapshot->entries[1].location.relativePath(), "real.desktop");
    EXPECT_EQ(completed.snapshot->entries[1].location.rootIndex(), 0U);
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 2U);
}

TEST(DesktopEntryDiscoveryTest, RejectsUnsafeOrOversizedSyntheticLocations) {
    const auto outOfRangeRoot = makeDiscoveredDesktopFileLocation(
        "/fixture/applications", "tool.desktop", maximumDesktopDiscoveryRoots);
    ASSERT_FALSE(outOfRangeRoot);
    EXPECT_EQ(outOfRangeRoot.error().code, ErrorCode::invalid_argument);

    const auto traversal = makeDiscoveredDesktopFileLocation(
        "/fixture/applications", "nested/../private-traversal.desktop", 0U);
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(traversal.error().message.find("private-traversal"), std::string::npos);

    const std::string controlledRelative{"private-control\n.desktop"};
    const auto relativeControl =
        makeDiscoveredDesktopFileLocation("/fixture/applications", controlledRelative, 0U);
    ASSERT_FALSE(relativeControl);
    EXPECT_EQ(relativeControl.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(relativeControl.error().message.find("private-control"), std::string::npos);

    const std::filesystem::path controlledRoot{std::string{"/private-control\nroot"}};
    const auto rootControl = makeDiscoveredDesktopFileLocation(controlledRoot, "tool.desktop", 0U);
    ASSERT_FALSE(rootControl);
    EXPECT_EQ(rootControl.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(rootControl.error().message.find("private-control"), std::string::npos);

    const std::string nulRelative{"private-nul\0.desktop", 20U};
    const auto nul = makeDiscoveredDesktopFileLocation("/fixture/applications", nulRelative, 0U);
    ASSERT_FALSE(nul);
    EXPECT_EQ(nul.error().code, ErrorCode::invalid_argument);

    const std::filesystem::path oversizedRoot{
        "/" + std::string(maximumDiscoveredDesktopFileLocationBytes, 'a')};
    const auto oversized = makeDiscoveredDesktopFileLocation(oversizedRoot, "tool.desktop", 0U);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopEntryDiscoveryTest, CancellationStopsBeforeWorkAndAFreshTokenResumes) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "applications";
    writeFile(root / "tool.desktop", application("Tool"));
    auto discovery = makeDiscovery({root});

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.requestCancellation());
    const auto stopped = discovery.pull(20U, cancelled.token());
    ASSERT_TRUE(stopped);
    EXPECT_TRUE(stopped.value().cancellationObserved);
    EXPECT_EQ(stopped.value().workUnits, 0U);
    EXPECT_FALSE(stopped.value().snapshot->complete);
    const auto oldSnapshot = stopped.value().snapshot;

    CancellationSource active;
    DesktopEntryDiscoveryBatch resumed;
    do {
        auto result = discovery.pull(1U, active.token());
        ASSERT_TRUE(result);
        resumed = std::move(result).value();
        EXPECT_LE(resumed.workUnits, 1U);
    } while (!resumed.snapshot->complete);

    ASSERT_EQ(resumed.snapshot->entries.size(), 1U);
    EXPECT_FALSE(oldSnapshot->complete);
    EXPECT_TRUE(oldSnapshot->entries.empty());
}

TEST(DesktopEntryDiscoveryTest, BatchReportsOnlyNewImmutableSnapshotIndices) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "applications";
    writeFile(root / "a.desktop", application("A"));
    writeFile(root / "b.desktop", application("B"));
    auto discovery = makeDiscovery({root});
    CancellationSource cancellation;

    std::shared_ptr<const DesktopEntryDiscoverySnapshot> beforeEntry;
    DesktopEntryDiscoveryBatch firstEntryBatch;
    for (;;) {
        auto result = discovery.pull(1U, cancellation.token());
        ASSERT_TRUE(result);
        auto batch = std::move(result).value();
        if (!batch.addedEntryIndices.empty()) {
            firstEntryBatch = std::move(batch);
            beforeEntry = firstEntryBatch.snapshot;
            break;
        }
    }
    EXPECT_EQ(firstEntryBatch.addedEntryIndices, (std::vector<std::size_t>{0U}));
    EXPECT_EQ(firstEntryBatch.addedVisibleEntryIndices, (std::vector<std::size_t>{0U}));

    DesktopEntryDiscoveryBatch secondEntryBatch;
    for (;;) {
        auto result = discovery.pull(1U, cancellation.token());
        ASSERT_TRUE(result);
        secondEntryBatch = std::move(result).value();
        if (!secondEntryBatch.addedEntryIndices.empty()) {
            break;
        }
    }
    EXPECT_EQ(secondEntryBatch.addedEntryIndices, (std::vector<std::size_t>{1U}));
    EXPECT_EQ(secondEntryBatch.addedVisibleEntryIndices, (std::vector<std::size_t>{1U}));
    EXPECT_EQ(beforeEntry->entries.size(), 1U);
    EXPECT_EQ(secondEntryBatch.snapshot->entries.size(), 2U);
}

TEST(DesktopEntryDiscoveryTest, CandidateLimitKeepsDeterministicLexicalPrefixAndStopsLowerRoots) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "z.desktop", application("Z"));
    writeFile(high / "a.desktop", application("A"));
    writeFile(low / "lower.desktop", application("Must not surface"));
    DesktopEntryDiscoveryLimits limits;
    limits.candidatesPerRoot = 1U;

    auto discovery = makeDiscovery({high, low}, limits);
    const auto completed = runToCompletion(discovery);

    EXPECT_TRUE(completed.snapshot->complete);
    EXPECT_TRUE(completed.snapshot->truncated);
    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "a.desktop");
    EXPECT_TRUE(hasDiagnostic(*completed.snapshot,
                              DesktopEntryDiscoveryDiagnosticCode::candidateLimitReached));
}

TEST(DesktopEntryDiscoveryTest, NodeOverflowPublishesNoEnumerationDependentPartialRoot) {
    TemporaryDirectory temporary;
    DesktopEntryDiscoveryLimits limits;
    limits.nodesPerRoot = 2U;
    const auto lowerRoot = temporary.path() / "lower";
    writeFile(lowerRoot / "lower.desktop", application("Must not surface"));

    const auto reverseRoot = temporary.path() / "reverse";
    writeFile(reverseRoot / "z.desktop", application("Z"));
    writeFile(reverseRoot / "m.desktop", application("M"));
    writeFile(reverseRoot / "a.desktop", application("A"));
    auto reverseDiscovery = makeDiscovery({reverseRoot, lowerRoot}, limits);
    const auto reverse = runToCompletion(reverseDiscovery, 1U);

    const auto forwardRoot = temporary.path() / "forward";
    writeFile(forwardRoot / "a.desktop", application("A"));
    writeFile(forwardRoot / "m.desktop", application("M"));
    writeFile(forwardRoot / "z.desktop", application("Z"));
    auto forwardDiscovery = makeDiscovery({forwardRoot, lowerRoot}, limits);
    const auto forward = runToCompletion(forwardDiscovery, 1U);

    EXPECT_TRUE(reverse.snapshot->truncated);
    EXPECT_TRUE(forward.snapshot->truncated);
    EXPECT_TRUE(reverse.snapshot->entries.empty());
    EXPECT_TRUE(forward.snapshot->entries.empty());
    EXPECT_EQ(reverse.snapshot->claimedDesktopFileIds, 0U);
    EXPECT_EQ(forward.snapshot->claimedDesktopFileIds, 0U);
    EXPECT_TRUE(
        hasDiagnostic(*reverse.snapshot, DesktopEntryDiscoveryDiagnosticCode::nodeLimitReached));
    EXPECT_TRUE(
        hasDiagnostic(*forward.snapshot, DesktopEntryDiscoveryDiagnosticCode::nodeLimitReached));
}

TEST(DesktopEntryDiscoveryTest, DirectoryMutationPublishesNoPartialRootOrLowerFallback) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    const auto external = temporary.path() / "external";
    std::filesystem::create_directories(high / "nested");
    writeFile(high / "visible.desktop", application("Must be discarded"));
    writeFile(low / "lower.desktop", application("Must remain suppressed"));
    writeFile(external / "escaped.desktop", application("Must not escape"));

    auto discovery = makeDiscovery({high, low});
    CancellationSource cancellation;
    const auto queued = discovery.pull(8U, cancellation.token());
    ASSERT_TRUE(queued);
    ASSERT_FALSE(queued.value().snapshot->complete);

    std::filesystem::remove(high / "nested");
    std::filesystem::create_directory_symlink(external, high / "nested");
    const auto completed = runToCompletion(discovery, 1U);

    EXPECT_TRUE(completed.snapshot->complete);
    EXPECT_TRUE(completed.snapshot->truncated);
    EXPECT_TRUE(completed.snapshot->entries.empty());
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 0U);
    EXPECT_TRUE(hasDiagnostic(*completed.snapshot,
                              DesktopEntryDiscoveryDiagnosticCode::directoryUnavailable));
}

TEST(DesktopEntryDiscoveryTest, EntryLimitPublishesBoundedPrefixAndStopsLowerRoots) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "a.desktop", application("A"));
    writeFile(high / "b.desktop", application("B"));
    writeFile(low / "lower.desktop", application("Must not surface"));
    DesktopEntryDiscoveryLimits limits;
    limits.entries = 1U;

    auto discovery = makeDiscovery({high, low}, limits);
    const auto completed = runToCompletion(discovery);

    EXPECT_TRUE(completed.snapshot->complete);
    EXPECT_TRUE(completed.snapshot->truncated);
    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "a.desktop");
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 2U);
    EXPECT_TRUE(
        hasDiagnostic(*completed.snapshot, DesktopEntryDiscoveryDiagnosticCode::entryLimitReached));
}

TEST(DesktopEntryDiscoveryTest, OversizedHigherFileShadowsLowerEntryWithClosedDiagnostic) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "tool.desktop", std::string(maximumDesktopEntryFileBytes + 1U, 'x'));
    writeFile(low / "tool.desktop", application("Must remain shadowed"));

    auto discovery = makeDiscovery({high, low});
    const auto completed = runToCompletion(discovery);

    EXPECT_TRUE(completed.snapshot->complete);
    EXPECT_TRUE(completed.snapshot->entries.empty());
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 1U);
    EXPECT_TRUE(
        hasDiagnostic(*completed.snapshot, DesktopEntryDiscoveryDiagnosticCode::fileTooLarge));
}

TEST(DesktopEntryDiscoveryTest, DepthBoundDoesNotFollowSkippedSubtreeOrLowerRoots) {
    TemporaryDirectory temporary;
    const auto high = temporary.path() / "high";
    const auto low = temporary.path() / "low";
    writeFile(high / "top.desktop", application("Top"));
    writeFile(high / "nested" / "deep.desktop", application("Deep"));
    writeFile(low / "lower.desktop", application("Must not surface"));
    DesktopEntryDiscoveryLimits limits;
    limits.depth = 1U;

    auto discovery = makeDiscovery({high, low}, limits);
    const auto completed = runToCompletion(discovery);

    EXPECT_TRUE(completed.snapshot->truncated);
    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_EQ(completed.snapshot->entries[0].id.value(), "top.desktop");
    EXPECT_TRUE(
        hasDiagnostic(*completed.snapshot, DesktopEntryDiscoveryDiagnosticCode::depthLimitReached));
}

TEST(DesktopEntryDiscoveryTest, DiagnosticsAreBoundedClosedAndContainNoPrivateData) {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "private-root-sentinel";
    writeFile(root / "private-entry-one.desktop", "private contents one");
    writeFile(root / "private-entry-two.desktop", "private contents two");
    DesktopEntryDiscoveryLimits limits;
    limits.diagnostics = 1U;

    auto discovery = makeDiscovery({root}, limits);
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->diagnostics.size(), 1U);
    EXPECT_TRUE(completed.snapshot->diagnosticsTruncated);
    EXPECT_EQ(completed.snapshot->diagnostics.front().code,
              DesktopEntryDiscoveryDiagnosticCode::desktopEntryRejected);
    EXPECT_EQ(completed.snapshot->diagnostics.front().rootIndex, 0U);
    EXPECT_EQ(completed.snapshot->claimedDesktopFileIds, 2U);
}

TEST(DesktopEntryDiscoveryTest, RejectsInvalidInputsWithStaticRedactedErrors) {
    DesktopEntryDiscoveryLimits zero;
    zero.nodesPerRoot = 0U;
    auto invalidLimit =
        createDesktopEntryDiscovery(ApplicationPaths{}, {"C"}, desktopContext(), zero);
    ASSERT_FALSE(invalidLimit);
    EXPECT_EQ(invalidLimit.error().code, ErrorCode::invalid_argument);

    auto invalidRoot = createDesktopEntryDiscovery(
        ApplicationPaths{{"relative/private-root-sentinel"}}, {"C"}, desktopContext(), {});
    ASSERT_FALSE(invalidRoot);
    EXPECT_EQ(invalidRoot.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(invalidRoot.error().message.find("private-root-sentinel"), std::string::npos);
    EXPECT_EQ(invalidRoot.error().recovery.find("private-root-sentinel"), std::string::npos);

    auto empty = makeDiscovery({});
    CancellationSource cancellation;
    auto zeroBudget = empty.pull(0U, cancellation.token());
    ASSERT_FALSE(zeroBudget);
    EXPECT_EQ(zeroBudget.error().code, ErrorCode::invalid_argument);
}

TEST(DesktopEntryDiscoveryTest, SilentlySkipsMissingStandardRoots) {
    TemporaryDirectory temporary;
    const auto missing = temporary.path() / "missing-standard-root";
    const auto available = temporary.path() / "available";
    writeFile(available / "tool.desktop", application("Tool"));

    auto discovery = makeDiscovery({missing, available});
    const auto completed = runToCompletion(discovery);

    ASSERT_EQ(completed.snapshot->entries.size(), 1U);
    EXPECT_TRUE(completed.snapshot->diagnostics.empty());
    EXPECT_FALSE(completed.snapshot->truncated);
}

} // namespace
} // namespace prismdrake::launcher
