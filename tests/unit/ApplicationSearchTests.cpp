#include "ApplicationSearch.hpp"

#include "DesktopEntryParser.hpp"
#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::CancellationSource;
using foundation::ErrorCode;

[[nodiscard]] DiscoveredDesktopEntry discovered(std::string id, std::string name,
                                                std::string genericName = {},
                                                std::vector<std::string> keywords = {},
                                                std::vector<std::string> categories = {}) {
    auto identifier = deriveDesktopFileId(id);
    EXPECT_TRUE(identifier);
    DesktopEntry entry;
    entry.name = std::move(name);
    if (!genericName.empty()) {
        entry.genericName = std::move(genericName);
    }
    entry.keywords = std::move(keywords);
    entry.categories = std::move(categories);
    entry.exec = "tool";
    return {std::move(identifier).value(), std::move(entry),
            DesktopEntryVisibilityReason::visibleByDefault, 0U};
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

[[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot>
catalog(std::shared_ptr<const DesktopEntryDiscoverySnapshot> source,
        std::uint64_t generation = 7U) {
    std::vector<ApplicationCatalogDecision> decisions;
    decisions.reserve(source->visibleEntryIndices.size());
    for (const auto index : source->visibleEntryIndices) {
        decisions.push_back({index, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec});
    }
    return std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
        generation, source, std::move(decisions), source->visibleEntryIndices,
        source->visibleEntryIndices.size(), source->visibleEntryIndices.size(), source->complete});
}

[[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot>
catalog(std::vector<DiscoveredDesktopEntry> entries, std::vector<std::size_t> visible = {},
        bool complete = true, std::uint64_t generation = 7U) {
    return catalog(discovery(std::move(entries), std::move(visible), complete), generation);
}

[[nodiscard]] ApplicationSearchQuery query(std::string_view text) {
    auto result = parseApplicationSearchQuery(text);
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return std::move(result).value();
}

[[nodiscard]] ApplicationSearchOperation
operation(std::shared_ptr<const ApplicationCatalogSnapshot> source, std::string_view text,
          std::size_t resultLimit = maximumApplicationSearchResults,
          std::uint64_t requestGeneration = 11U) {
    auto result =
        createApplicationSearch(std::move(source), requestGeneration, query(text), resultLimit);
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return std::move(result).value();
}

[[nodiscard]] std::shared_ptr<const ApplicationSearchSnapshot>
advance(ApplicationSearchOperation &search,
        std::size_t budget = maximumApplicationSearchWorkUnits) {
    CancellationSource cancellation;
    auto result = search.advance(budget, cancellation.token());
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    return result ? std::move(result).value() : nullptr;
}

[[nodiscard]] std::shared_ptr<const ApplicationSearchSnapshot>
complete(ApplicationSearchOperation &search, std::size_t budget = 3U) {
    for (std::size_t iteration = 0U; iteration <= maximumDesktopDiscoveryEntries; ++iteration) {
        auto snapshot = advance(search, budget);
        if (!snapshot || snapshot->state != ApplicationSearchViewState::loading ||
            snapshot->examinedApplications == snapshot->totalApplications) {
            return snapshot;
        }
    }
    ADD_FAILURE() << "application search did not complete within its catalog bound";
    return nullptr;
}

[[nodiscard]] std::vector<std::string> resultIds(const ApplicationSearchSnapshot &search,
                                                 const ApplicationCatalogSnapshot &source) {
    std::vector<std::string> ids;
    for (const auto &result : search.results) {
        ids.push_back(source.discovery->entries[result.discoveryEntryIndex].id.value());
    }
    return ids;
}

TEST(ApplicationSearchTest, MatchesEverySelectedLocalApplicationField) {
    const auto source = catalog({discovered("name.desktop", "Dragon Painter"),
                                 discovered("generic.desktop", "Forge", "Image Editor"),
                                 discovered("keyword.desktop", "Lustre", {}, {"prismatic"}),
                                 discovered("category.desktop", "Scale", {}, {}, {"Graphics"})});

    for (const auto &[text, expected] :
         std::vector<std::pair<std::string, std::string>>{{"dragon", "name.desktop"},
                                                          {"editor", "generic.desktop"},
                                                          {"prismatic", "keyword.desktop"},
                                                          {"graphics", "category.desktop"}}) {
        auto search = operation(source, text);
        const auto result = complete(search);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->state, ApplicationSearchViewState::results);
        EXPECT_EQ(resultIds(*result, *source), (std::vector<std::string>{expected}));
    }
}

TEST(ApplicationSearchTest, SearchesOnlyPublishedVisibleEntryIndices) {
    const auto source = catalog({discovered("visible.desktop", "Visible Match"),
                                 discovered("unpublished.desktop", "Private Match")},
                                {0U});
    auto visible = operation(source, "visible");
    EXPECT_EQ(complete(visible)->state, ApplicationSearchViewState::results);

    auto unpublished = operation(source, "private");
    EXPECT_EQ(complete(unpublished)->state, ApplicationSearchViewState::noResults);
}

TEST(ApplicationSearchTest, CannotSearchTryExecExcludedEntries) {
    const auto source = discovery({discovered("eligible.desktop", "Eligible Dragon"),
                                   discovered("excluded.desktop", "Excluded Secret")});
    const auto eligibleCatalog =
        std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
            19U,
            source,
            {{0U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec},
             {1U, ApplicationCatalogEligibilityReason::excludedTryExecMissing}},
            {0U},
            2U,
            2U,
            true});

    auto excluded = operation(eligibleCatalog, "secret");
    const auto excludedResult = complete(excluded);
    ASSERT_NE(excludedResult, nullptr);
    EXPECT_EQ(excludedResult->state, ApplicationSearchViewState::noResults);
    EXPECT_TRUE(excludedResult->results.empty());
    EXPECT_EQ(excludedResult->catalogGeneration, 19U);
    EXPECT_EQ(excludedResult->examinedApplications, 1U);
    EXPECT_EQ(excludedResult->totalApplications, 1U);

    auto eligible = operation(eligibleCatalog, "dragon");
    const auto eligibleResult = complete(eligible);
    ASSERT_NE(eligibleResult, nullptr);
    EXPECT_EQ(resultIds(*eligibleResult, *eligibleCatalog),
              (std::vector<std::string>{"eligible.desktop"}));
}

TEST(ApplicationSearchTest, SearchesParserSelectedLocalizedValuesAndBaseCategories) {
    constexpr std::string_view document = R"([Desktop Entry]
Type=Application
Name=Calculator
Name[fr]=Calculatrice
GenericName=Utility
GenericName[fr]=Utilitaire
Keywords=math;
Keywords[fr]=calcul;
Categories=Utility;
Exec=tool
)";
    const auto parsed = parseDesktopEntry(document, DesktopEntryParseContext{"fr_FR.UTF-8"});
    ASSERT_TRUE(parsed);
    auto identifier = deriveDesktopFileId("localized.desktop");
    ASSERT_TRUE(identifier);
    const auto source = catalog({{std::move(identifier).value(), parsed.value(),
                                  DesktopEntryVisibilityReason::visibleByDefault, 0U}});

    for (const std::string_view text : {"calculatrice", "utilitaire", "calcul", "utility"}) {
        auto search = operation(source, text);
        ASSERT_EQ(complete(search)->state, ApplicationSearchViewState::results) << text;
    }
    auto baseKeyword = operation(source, "math");
    EXPECT_EQ(complete(baseKeyword)->state, ApplicationSearchViewState::noResults);
}

TEST(ApplicationSearchTest, UsesAsciiFoldingAndExactNonAsciiUtf8WithoutCollationClaims) {
    const auto source = catalog({discovered("accent.desktop", "ÉLAN Dragon")});

    auto exactNonAscii = operation(source, "Élan");
    EXPECT_EQ(complete(exactNonAscii)->state, ApplicationSearchViewState::results);

    auto differentNonAsciiCase = operation(source, "élan");
    EXPECT_EQ(complete(differentNonAsciiCase)->state, ApplicationSearchViewState::noResults);
}

TEST(ApplicationSearchTest, RequiresEveryTokenAndUsesPhraseShapeThenFieldRanking) {
    const auto source =
        catalog({discovered("name-exact.desktop", "Paint Tool"),
                 discovered("generic-exact.desktop", "Forge", "Paint Tool"),
                 discovered("keyword-exact.desktop", "Lustre", {}, {"Paint Tool"}),
                 discovered("category-exact.desktop", "Scale", {}, {}, {"Paint Tool"}),
                 discovered("name-prefix.desktop", "Paint Toolbox"),
                 discovered("word-prefix.desktop", "A Paint Toolset"),
                 discovered("substring.desktop", "Repaint Toolset"),
                 discovered("partial.desktop", "Paint Only")});
    auto search = operation(source, "paint tool");
    const auto result = complete(search);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(resultIds(*result, *source),
              (std::vector<std::string>{"name-exact.desktop", "generic-exact.desktop",
                                        "keyword-exact.desktop", "category-exact.desktop",
                                        "name-prefix.desktop", "word-prefix.desktop",
                                        "substring.desktop"}));
}

TEST(ApplicationSearchTest, TieBreaksByFoldedLocalizedNameThenDesktopFileId) {
    const auto source = catalog({discovered("z.desktop", "Beta", {}, {"match"}),
                                 discovered("b.desktop", "alpha", {}, {"match"}),
                                 discovered("a.desktop", "Alpha", {}, {"match"})});
    auto search = operation(source, "match");
    const auto result = complete(search);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(resultIds(*result, *source),
              (std::vector<std::string>{"a.desktop", "b.desktop", "z.desktop"}));
}

TEST(ApplicationSearchTest, FinalOrderIsIndependentOfVisibleOrderAndSliceSize) {
    std::vector<DiscoveredDesktopEntry> entries;
    for (std::size_t index = 0U; index < 12U; ++index) {
        entries.push_back(discovered("app-" + std::to_string(index) + ".desktop",
                                     "Matching App " + std::to_string(11U - index)));
    }
    const auto ordered = catalog(entries);
    const auto shuffled =
        catalog(std::move(entries), {7U, 0U, 11U, 2U, 5U, 9U, 1U, 10U, 3U, 8U, 4U, 6U});
    auto oneAtATime = operation(ordered, "matching");
    auto oneBatch = operation(shuffled, "matching");
    const auto first = complete(oneAtATime, 1U);
    const auto second = complete(oneBatch, maximumApplicationSearchWorkUnits);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(resultIds(*first, *ordered), resultIds(*second, *shuffled));
}

TEST(ApplicationSearchTest, PublishesLoadingEmptyNoResultResultAndErrorStates) {
    const auto partial = catalog({discovered("partial.desktop", "Match")}, {}, false);
    auto partialSearch = operation(partial, "match");
    const auto loading = advance(partialSearch);
    ASSERT_NE(loading, nullptr);
    EXPECT_EQ(loading->state, ApplicationSearchViewState::loading);
    ASSERT_EQ(loading->results.size(), 1U);

    auto hiddenEntry = discovered("hidden.desktop", "Hidden");
    hiddenEntry.visibility = DesktopEntryVisibilityReason::hiddenNoDisplay;
    const auto empty = catalog(std::make_shared<const DesktopEntryDiscoverySnapshot>(
        DesktopEntryDiscoverySnapshot{{std::move(hiddenEntry)}, {}, {}, 0U, true, false, false}));
    auto emptySearch = operation(empty, "anything");
    EXPECT_EQ(advance(emptySearch)->state, ApplicationSearchViewState::emptyCatalog);

    const auto populated = catalog({discovered("one.desktop", "One")});
    auto absentSearch = operation(populated, "absent");
    EXPECT_EQ(complete(absentSearch)->state, ApplicationSearchViewState::noResults);
    auto resultSearch = operation(populated, "one");
    EXPECT_EQ(complete(resultSearch)->state, ApplicationSearchViewState::results);

    const auto error = makeApplicationSearchErrorSnapshot(9U, 4U);
    ASSERT_TRUE(error);
    EXPECT_EQ(error.value()->state, ApplicationSearchViewState::error);
    EXPECT_TRUE(error.value()->results.empty());
}

TEST(ApplicationSearchTest, CancellationNeverPublishesAStaleCompletion) {
    const auto source =
        catalog({discovered("one.desktop", "Match One"), discovered("two.desktop", "Match Two")});
    auto search = operation(source, "match");
    const auto provisional = advance(search, 1U);
    ASSERT_NE(provisional, nullptr);
    ASSERT_EQ(provisional->state, ApplicationSearchViewState::loading);
    ASSERT_EQ(provisional->examinedApplications, 1U);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.requestCancellation());
    const auto cancelled = search.advance(1U, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::cancelled);
    EXPECT_EQ(provisional->examinedApplications, 1U);
    EXPECT_EQ(provisional->state, ApplicationSearchViewState::loading);

    CancellationSource current;
    const auto staleResume = search.advance(1U, current.token());
    ASSERT_FALSE(staleResume);
    EXPECT_EQ(staleResume.error().code, ErrorCode::cancelled);

    auto replacement = operation(source, "match", maximumApplicationSearchResults, 12U);
    const auto completed = complete(replacement);
    ASSERT_NE(completed, nullptr);
    EXPECT_EQ(completed->state, ApplicationSearchViewState::results);
    EXPECT_EQ(completed->requestGeneration, 12U);
}

TEST(ApplicationSearchTest, AcceptsExactQueryAndOperationLimitsAndRejectsOneOver) {
    std::string exactCodepoints;
    for (std::size_t index = 0U; index < maximumApplicationSearchQueryCodepoints; ++index) {
        exactCodepoints.append("🐉");
    }
    ASSERT_EQ(exactCodepoints.size(), maximumApplicationSearchQueryBytes);
    EXPECT_TRUE(parseApplicationSearchQuery(exactCodepoints));
    exactCodepoints.push_back('a');
    const auto tooLong = parseApplicationSearchQuery(exactCodepoints);
    ASSERT_FALSE(tooLong);
    EXPECT_EQ(tooLong.error().code, ErrorCode::too_large);

    std::string exactAsciiCodepoints(maximumApplicationSearchQueryCodepoints, 'a');
    EXPECT_TRUE(parseApplicationSearchQuery(exactAsciiCodepoints));
    exactAsciiCodepoints.push_back('a');
    const auto tooManyCodepoints = parseApplicationSearchQuery(exactAsciiCodepoints);
    ASSERT_FALSE(tooManyCodepoints);
    EXPECT_EQ(tooManyCodepoints.error().code, ErrorCode::too_large);

    std::string exactTokens;
    for (std::size_t index = 0U; index < maximumApplicationSearchQueryTokens; ++index) {
        if (!exactTokens.empty()) {
            exactTokens.push_back(' ');
        }
        exactTokens.push_back('a');
    }
    EXPECT_TRUE(parseApplicationSearchQuery(exactTokens));
    const auto tooManyTokens = parseApplicationSearchQuery(exactTokens + " a");
    ASSERT_FALSE(tooManyTokens);
    EXPECT_EQ(tooManyTokens.error().code, ErrorCode::too_large);

    const auto source = catalog({discovered("one.desktop", "One")});
    EXPECT_TRUE(createApplicationSearch(source, 1U, query("one"), maximumApplicationSearchResults));
    EXPECT_FALSE(
        createApplicationSearch(source, 0U, query("one"), maximumApplicationSearchResults));
    auto zeroGeneration = std::make_shared<ApplicationCatalogSnapshot>(*source);
    zeroGeneration->generation = 0U;
    EXPECT_FALSE(createApplicationSearch(std::move(zeroGeneration), 1U, query("one"),
                                         maximumApplicationSearchResults));
    EXPECT_FALSE(
        createApplicationSearch(source, 1U, query("one"), maximumApplicationSearchResults + 1U));
    auto search = operation(source, "one");
    CancellationSource cancellation;
    EXPECT_TRUE(search.advance(maximumApplicationSearchWorkUnits, cancellation.token()));
    EXPECT_FALSE(search.advance(maximumApplicationSearchWorkUnits + 1U, cancellation.token()));
    EXPECT_FALSE(search.advance(0U, cancellation.token()));
}

TEST(ApplicationSearchTest, RejectsInvalidInputWithStaticRedactedFailures) {
    std::string privateInvalid = "private-search";
    privateInvalid.push_back(static_cast<char>(0xFF));
    const auto invalidUtf8 = parseApplicationSearchQuery(privateInvalid);
    ASSERT_FALSE(invalidUtf8);
    EXPECT_EQ(invalidUtf8.error().code, ErrorCode::validation_error);
    EXPECT_EQ(invalidUtf8.error().message.find("private-search"), std::string::npos);
    EXPECT_EQ(invalidUtf8.error().recovery.find("private-search"), std::string::npos);

    const std::string privateNull{"private\0search", 14U};
    const auto invalidNull = parseApplicationSearchQuery(privateNull);
    ASSERT_FALSE(invalidNull);
    EXPECT_EQ(invalidNull.error().message.find("private"), std::string::npos);

    const auto invalidDiscovery = std::make_shared<const DesktopEntryDiscoverySnapshot>(
        DesktopEntryDiscoverySnapshot{{}, {99U}, {}, 0U, true, false, false});
    auto invalidCatalog =
        std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
            1U,
            invalidDiscovery,
            {{99U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec}},
            {99U},
            1U,
            1U,
            true});
    const auto invalidOperation =
        createApplicationSearch(std::move(invalidCatalog), 1U, query("private"), 1U);
    ASSERT_FALSE(invalidOperation);
    EXPECT_EQ(invalidOperation.error().message.find("private"), std::string::npos);
}

TEST(ApplicationSearchTest, RejectsInconsistentCatalogPublicationShapes) {
    const auto source =
        discovery({discovered("one.desktop", "One"), discovered("two.desktop", "Two")});
    const ApplicationCatalogSnapshot valid{
        5U,
        source,
        {{0U, ApplicationCatalogEligibilityReason::eligibleWithoutTryExec},
         {1U, ApplicationCatalogEligibilityReason::excludedTryExecNotExecutable}},
        {0U},
        2U,
        2U,
        true};
    const auto expectRejected = [](ApplicationCatalogSnapshot candidate) {
        const auto result = createApplicationSearch(
            std::make_shared<const ApplicationCatalogSnapshot>(std::move(candidate)), 1U,
            query("one"), 1U);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::validation_error);
    };

    auto missingDiscovery = valid;
    missingDiscovery.discovery.reset();
    expectRejected(std::move(missingDiscovery));

    auto countMismatch = valid;
    countMismatch.examinedEntries = 1U;
    expectRejected(std::move(countMismatch));

    auto prefixMismatch = valid;
    std::swap(prefixMismatch.decisions[0], prefixMismatch.decisions[1]);
    expectRejected(std::move(prefixMismatch));

    auto unknownReason = valid;
    unknownReason.decisions[0].reason = static_cast<ApplicationCatalogEligibilityReason>(255U);
    expectRejected(std::move(unknownReason));

    auto eligibilityMismatch = valid;
    eligibilityMismatch.eligibleEntryIndices = {1U};
    expectRejected(std::move(eligibilityMismatch));

    auto completionMismatch = valid;
    completionMismatch.complete = false;
    expectRejected(std::move(completionMismatch));
}

TEST(ApplicationSearchTest, RejectsDuplicateIdsAndNonvisibleReferencedEntries) {
    auto first = discovered("duplicate.desktop", "First");
    auto second = discovered("duplicate.desktop", "Second");
    const auto duplicateCatalog = catalog({std::move(first), std::move(second)});
    const auto duplicate = createApplicationSearch(duplicateCatalog, 1U, query("entry"), 1U);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::validation_error);

    auto hidden = discovered("hidden.desktop", "Hidden");
    hidden.visibility = DesktopEntryVisibilityReason::hiddenNoDisplay;
    const auto hiddenCatalog = catalog({std::move(hidden)});
    const auto nonvisible = createApplicationSearch(hiddenCatalog, 1U, query("hidden"), 1U);
    ASSERT_FALSE(nonvisible);
    EXPECT_EQ(nonvisible.error().code, ErrorCode::validation_error);
}

TEST(ApplicationSearchTest, TruncatesToTheSameBestResultsDeterministically) {
    std::vector<DiscoveredDesktopEntry> entries{
        discovered("d.desktop", "Delta Match"), discovered("b.desktop", "Beta Match"),
        discovered("a.desktop", "Alpha Match"), discovered("c.desktop", "Charlie Match")};
    const auto firstSource = catalog(entries);
    const auto secondSource = catalog(std::move(entries), {3U, 1U, 0U, 2U});
    auto firstSearch = operation(firstSource, "match", 2U);
    auto secondSearch = operation(secondSource, "match", 2U);
    const auto first = complete(firstSearch, 1U);
    const auto second = complete(secondSearch, 4U);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->truncated);
    EXPECT_TRUE(second->truncated);
    EXPECT_EQ(resultIds(*first, *firstSource),
              (std::vector<std::string>{"a.desktop", "b.desktop"}));
    EXPECT_EQ(resultIds(*first, *firstSource), resultIds(*second, *secondSource));
}

TEST(ApplicationSearchTest, EmptyQueryUsesDeterministicNameAndIdentifierOrdering) {
    const auto source = catalog({discovered("z.desktop", "Beta"), discovered("b.desktop", "alpha"),
                                 discovered("a.desktop", "Alpha")},
                                {2U, 0U, 1U});
    auto search = operation(source, " \t\n ");
    const auto result = complete(search);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(resultIds(*result, *source),
              (std::vector<std::string>{"a.desktop", "b.desktop", "z.desktop"}));
}

} // namespace
} // namespace prismdrake::launcher
