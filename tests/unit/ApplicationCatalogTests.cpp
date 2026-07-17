#include "ApplicationCatalog.hpp"
#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::launcher {
namespace {

using foundation::CancellationSource;
using foundation::ErrorCode;

class CatalogExecutableTree final {
  public:
    CatalogExecutableTree() {
        std::string pattern = "/tmp/prismdrake-application-catalog-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create an application catalog fixture."};
        }
        root_ = created;
    }

    ~CatalogExecutableTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    CatalogExecutableTree(const CatalogExecutableTree &) = delete;
    CatalogExecutableTree &operator=(const CatalogExecutableTree &) = delete;

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }

    std::filesystem::path write(std::filesystem::path relative, bool executable = true) const {
        const auto path = root_ / std::move(relative);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error{"Could not create an application catalog fixture file."};
        }
        output << "fixture\n";
        output.close();
        if (::chmod(path.c_str(), executable ? 0700U : 0600U) != 0) {
            throw std::runtime_error{"Could not set application catalog fixture permissions."};
        }
        return path;
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] DiscoveredDesktopEntry discovered(
    std::string id, std::optional<std::string> tryExec = std::nullopt,
    DesktopEntryVisibilityReason visibility = DesktopEntryVisibilityReason::visibleByDefault) {
    auto identifier = deriveDesktopFileId(id);
    EXPECT_TRUE(identifier);
    DesktopEntry entry;
    entry.name = "Tool";
    entry.exec = "private;actual-exec-is-not-evaluated";
    entry.tryExec = std::move(tryExec);
    return {std::move(identifier).value(), std::move(entry), visibility, 0U};
}

[[nodiscard]] std::shared_ptr<const DesktopEntryDiscoverySnapshot>
discovery(std::vector<DiscoveredDesktopEntry> entries, std::vector<std::size_t> visible = {},
          bool complete = true) {
    if (visible.empty() && !entries.empty()) {
        visible.resize(entries.size());
        for (std::size_t index = 0U; index < visible.size(); ++index) {
            visible[index] = index;
        }
    }
    return std::make_shared<const DesktopEntryDiscoverySnapshot>(DesktopEntryDiscoverySnapshot{
        std::move(entries), std::move(visible), {}, 0U, complete, false, false});
}

[[nodiscard]] DesktopExecutableLookupContext lookup(const CatalogExecutableTree &tree) {
    return {(tree.root() / "bin").string(), tree.root()};
}

[[nodiscard]] ApplicationCatalogOperation
operation(std::shared_ptr<const DesktopEntryDiscoverySnapshot> source,
          DesktopExecutableLookupContext context, std::uint64_t generation = 7U) {
    auto result = createApplicationCatalog(std::move(source), std::move(context), generation);
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return std::move(result).value();
}

[[nodiscard]] ApplicationCatalogBatch
pull(ApplicationCatalogOperation &catalog,
     std::size_t budget = maximumApplicationCatalogWorkUnits) {
    CancellationSource cancellation;
    auto result = catalog.pull(budget, cancellation.token());
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return result ? std::move(result).value() : ApplicationCatalogBatch{};
}

[[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot>
complete(ApplicationCatalogOperation &catalog, std::size_t budget = 2U) {
    for (std::size_t iteration = 0U; iteration <= maximumDesktopDiscoveryEntries; ++iteration) {
        auto batch = pull(catalog, budget);
        if (!batch.snapshot ||
            batch.snapshot->examinedEntries == batch.snapshot->totalVisibleEntries) {
            return batch.snapshot;
        }
    }
    ADD_FAILURE() << "application catalog did not consume its bounded discovery snapshot";
    return nullptr;
}

TEST(ApplicationCatalogTest, IncludesAndExcludesTryExecWithClosedReasonsInVisibleOrder) {
    CatalogExecutableTree tree;
    tree.write("bin/available");
    tree.write("bin/disabled", false);
    std::filesystem::create_directories(tree.root() / "bin" / "directory");
    const auto source = discovery(
        {discovered("absent.desktop"), discovered("available.desktop", "available"),
         discovered("missing.desktop", "missing"), discovered("directory.desktop", "directory"),
         discovered("disabled.desktop", "disabled")},
        {4U, 0U, 3U, 1U, 2U});
    auto catalog = operation(source, lookup(tree));

    const auto snapshot = complete(catalog, 1U);

    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->generation, 7U);
    EXPECT_EQ(snapshot->discovery, source);
    EXPECT_TRUE(snapshot->complete);
    EXPECT_EQ(snapshot->examinedEntries, 5U);
    EXPECT_EQ(snapshot->totalVisibleEntries, 5U);
    EXPECT_EQ(snapshot->decisions,
              (std::vector<ApplicationCatalogDecision>{
                  {4U, ApplicationCatalogEligibilityReason::excludedTryExecNotExecutable},
                  {0U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec},
                  {3U, ApplicationCatalogEligibilityReason::excludedTryExecNotRegularFile},
                  {1U, ApplicationCatalogEligibilityReason::eligibleTryExec},
                  {2U, ApplicationCatalogEligibilityReason::excludedTryExecMissing}}));
    EXPECT_EQ(snapshot->eligibleEntryIndices, (std::vector<std::size_t>{0U, 1U}));
    for (const auto &decision : snapshot->decisions) {
        EXPECT_EQ(isCatalogEligible(decision.reason),
                  decision.discoveryEntryIndex == 0U || decision.discoveryEntryIndex == 1U);
    }
}

TEST(ApplicationCatalogTest, IgnoresActualExecAndProcessesOnlyPublishedVisibleIndices) {
    CatalogExecutableTree tree;
    auto hidden =
        discovered("hidden.desktop", std::nullopt, DesktopEntryVisibilityReason::hiddenNoDisplay);
    hidden.entry.tryExec = "relative/private-hidden-sentinel";
    const auto source = discovery({discovered("visible.desktop"), std::move(hidden)}, {0U});
    auto catalog = operation(source, lookup(tree));

    const auto snapshot = complete(catalog);

    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->decisions.size(), 1U);
    EXPECT_EQ(snapshot->decisions.front().discoveryEntryIndex, 0U);
    EXPECT_EQ(snapshot->decisions.front().reason,
              ApplicationCatalogEligibilityReason::eligibleWithoutTryExec);
}

TEST(ApplicationCatalogTest, PublishesImmutableCumulativeSnapshots) {
    CatalogExecutableTree tree;
    const auto source = discovery(
        {discovered("one.desktop"), discovered("two.desktop"), discovered("three.desktop")});
    auto catalog = operation(source, lookup(tree));

    const auto first = pull(catalog, 1U);
    ASSERT_NE(first.snapshot, nullptr);
    ASSERT_EQ(first.snapshot->decisions.size(), 1U);
    EXPECT_EQ(first.workUnits, 1U);
    const auto firstPointer = first.snapshot;

    const auto second = pull(catalog, 1U);
    ASSERT_NE(second.snapshot, nullptr);
    EXPECT_NE(second.snapshot, firstPointer);
    EXPECT_EQ(second.snapshot->decisions.size(), 2U);
    EXPECT_EQ(firstPointer->decisions.size(), 1U);
    EXPECT_EQ(firstPointer->examinedEntries, 1U);

    const auto final = pull(catalog, maximumApplicationCatalogWorkUnits);
    ASSERT_NE(final.snapshot, nullptr);
    EXPECT_TRUE(final.snapshot->complete);
    const auto unchanged = pull(catalog, 1U);
    EXPECT_EQ(unchanged.workUnits, 0U);
    EXPECT_EQ(unchanged.snapshot, final.snapshot);
}

TEST(ApplicationCatalogTest, FinalResultIsIndependentOfPullSliceSize) {
    CatalogExecutableTree tree;
    tree.write("bin/available");
    tree.write("bin/disabled", false);
    const auto source =
        discovery({discovered("one.desktop"), discovered("two.desktop", "available"),
                   discovered("three.desktop", "missing"), discovered("four.desktop", "disabled")},
                  {2U, 0U, 3U, 1U});
    auto oneAtATime = operation(source, lookup(tree));
    auto oneBatch = operation(source, lookup(tree));

    const auto first = complete(oneAtATime, 1U);
    const auto second = complete(oneBatch, maximumApplicationCatalogWorkUnits);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->decisions, second->decisions);
    EXPECT_EQ(first->eligibleEntryIndices, second->eligibleEntryIndices);
    EXPECT_EQ(first->complete, second->complete);
}

TEST(ApplicationCatalogTest, CompletesOnlyForACompleteFullyExaminedDiscoverySnapshot) {
    CatalogExecutableTree tree;
    const auto partialSource = discovery({discovered("partial.desktop")}, {}, false);
    auto partial = operation(partialSource, lookup(tree));
    const auto partialSnapshot = complete(partial);
    ASSERT_NE(partialSnapshot, nullptr);
    EXPECT_EQ(partialSnapshot->examinedEntries, 1U);
    EXPECT_FALSE(partialSnapshot->complete);

    const auto emptySource = discovery({}, {}, true);
    auto empty = operation(emptySource, lookup(tree));
    const auto emptyBatch = pull(empty);
    ASSERT_NE(emptyBatch.snapshot, nullptr);
    EXPECT_EQ(emptyBatch.workUnits, 0U);
    EXPECT_TRUE(emptyBatch.snapshot->complete);
}

TEST(ApplicationCatalogTest, CancellationIsTerminalAndPreservesPublishedSnapshots) {
    CatalogExecutableTree tree;
    const auto source = discovery({discovered("one.desktop"), discovered("two.desktop")});
    auto catalog = operation(source, lookup(tree));
    const auto provisional = pull(catalog, 1U).snapshot;
    ASSERT_NE(provisional, nullptr);
    ASSERT_EQ(provisional->examinedEntries, 1U);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.requestCancellation());
    const auto cancelled = catalog.pull(1U, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::cancelled);
    EXPECT_EQ(provisional->examinedEntries, 1U);
    EXPECT_FALSE(provisional->complete);

    CancellationSource fresh;
    const auto staleResume = catalog.pull(1U, fresh.token());
    ASSERT_FALSE(staleResume);
    EXPECT_EQ(staleResume.error().code, ErrorCode::cancelled);
}

TEST(ApplicationCatalogTest, MalformedTryExecIsATerminalRedactedFailure) {
    CatalogExecutableTree tree;
    const auto source =
        discovery({discovered("private-entry.desktop", "relative/private-tryexec-sentinel")});
    auto catalog = operation(source, lookup(tree));
    CancellationSource cancellation;

    const auto failed = catalog.pull(1U, cancellation.token());

    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(failed.error().message.find("private-tryexec-sentinel"), std::string::npos);
    EXPECT_EQ(failed.error().recovery.find("private-tryexec-sentinel"), std::string::npos);
    const auto repeated = catalog.pull(1U, cancellation.token());
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error(), failed.error());
}

TEST(ApplicationCatalogTest, ResolverIoFailureTerminatesWithoutPublishingAFalseExclusion) {
    CatalogExecutableTree tree;
    const auto loop = tree.root() / "loop";
    std::filesystem::create_symlink(loop.filename(), loop);
    const auto source = discovery({discovered("loop.desktop", loop.string())});
    auto catalog = operation(source, lookup(tree));
    CancellationSource cancellation;

    const auto failed = catalog.pull(1U, cancellation.token());

    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::io_error);
}

TEST(ApplicationCatalogTest, RejectsInvalidDiscoveryShapeAndGeneration) {
    CatalogExecutableTree tree;
    EXPECT_FALSE(createApplicationCatalog(nullptr, lookup(tree), 1U));
    EXPECT_FALSE(createApplicationCatalog(discovery({}), lookup(tree), 0U));

    const auto outOfRange =
        std::make_shared<const DesktopEntryDiscoverySnapshot>(DesktopEntryDiscoverySnapshot{
            {discovered("one.desktop")}, {1U}, {}, 0U, true, false, false});
    const auto invalidIndex = createApplicationCatalog(outOfRange, lookup(tree), 1U);
    ASSERT_FALSE(invalidIndex);
    EXPECT_EQ(invalidIndex.error().code, ErrorCode::validation_error);

    const auto duplicateIndex =
        std::make_shared<const DesktopEntryDiscoverySnapshot>(DesktopEntryDiscoverySnapshot{
            {discovered("one.desktop")}, {0U, 0U}, {}, 0U, true, false, false});
    const auto duplicate = createApplicationCatalog(duplicateIndex, lookup(tree), 1U);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::validation_error);

    const auto hidden = std::make_shared<const DesktopEntryDiscoverySnapshot>(
        DesktopEntryDiscoverySnapshot{{discovered("hidden.desktop", std::nullopt,
                                                  DesktopEntryVisibilityReason::hiddenNoDisplay)},
                                      {0U},
                                      {},
                                      0U,
                                      true,
                                      false,
                                      false});
    const auto nonvisible = createApplicationCatalog(hidden, lookup(tree), 1U);
    ASSERT_FALSE(nonvisible);
    EXPECT_EQ(nonvisible.error().code, ErrorCode::validation_error);
}

TEST(ApplicationCatalogTest, RejectsDuplicateDesktopIdsAndOversizedSnapshotVectors) {
    CatalogExecutableTree tree;
    const auto duplicateIds =
        discovery({discovered("duplicate.desktop"), discovered("duplicate.desktop")}, {0U, 1U});
    const auto duplicate = createApplicationCatalog(duplicateIds, lookup(tree), 1U);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::validation_error);

    std::vector<std::size_t> oversizedVisible(maximumDesktopDiscoveryEntries + 1U, 0U);
    const auto oversized =
        std::make_shared<const DesktopEntryDiscoverySnapshot>(DesktopEntryDiscoverySnapshot{
            {discovered("one.desktop")}, std::move(oversizedVisible), {}, 0U, true, false, false});
    const auto tooLarge = createApplicationCatalog(oversized, lookup(tree), 1U);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, ErrorCode::too_large);
}

TEST(ApplicationCatalogTest, ValidatesLookupEnvelopeEvenForAnEmptyCatalog) {
    CatalogExecutableTree tree;
    DesktopExecutableLookupContext invalid{
        std::string(maximumDesktopExecutableSearchPathBytes + 1U, 'p'), tree.root()};

    const auto result = createApplicationCatalog(discovery({}), std::move(invalid), 1U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(ApplicationCatalogTest, EnforcesPullBoundsAndMovedFromOwnership) {
    CatalogExecutableTree tree;
    auto catalog = operation(discovery({discovered("one.desktop")}), lookup(tree));
    CancellationSource cancellation;
    EXPECT_FALSE(catalog.pull(0U, cancellation.token()));
    EXPECT_FALSE(catalog.pull(maximumApplicationCatalogWorkUnits + 1U, cancellation.token()));
    EXPECT_TRUE(catalog.pull(maximumApplicationCatalogWorkUnits, cancellation.token()));

    auto moved = std::move(catalog);
    const auto invalidOwner = catalog.pull(1U, cancellation.token());
    ASSERT_FALSE(invalidOwner);
    EXPECT_EQ(invalidOwner.error().code, ErrorCode::invalid_argument);
    EXPECT_TRUE(moved.pull(1U, cancellation.token()));
}

} // namespace
} // namespace prismdrake::launcher
