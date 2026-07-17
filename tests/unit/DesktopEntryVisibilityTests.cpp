#include "DesktopEntryVisibility.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

[[nodiscard]] DesktopEntry
entryWithVisibility(std::optional<std::vector<std::string>> onlyShowIn = std::nullopt,
                    std::optional<std::vector<std::string>> notShowIn = std::nullopt) {
    DesktopEntry entry;
    entry.onlyShowIn = std::move(onlyShowIn);
    entry.notShowIn = std::move(notShowIn);
    return entry;
}

[[nodiscard]] CurrentDesktopContext context(std::string_view value) {
    auto parsed = parseCurrentDesktopContext(value);
    return std::move(parsed).value();
}

TEST(DesktopEntryVisibilityTest, ParsesOrderedDesktopNamesAndIgnoresEmptyComponents) {
    const auto parsed = parseCurrentDesktopContext(":Prismdrake::Forge:Prismdrake:");

    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value().names(),
              (std::vector<std::string>{"Prismdrake", "Forge", "Prismdrake"}));
    EXPECT_TRUE(parseCurrentDesktopContext("").value().names().empty());
    EXPECT_TRUE(parseCurrentDesktopContext(":::").value().names().empty());
}

TEST(DesktopEntryVisibilityTest, RejectsOversizedNameListAndNullWithStaticErrors) {
    std::string exactComponent(maximumCurrentDesktopEntryBytes, 'x');
    ASSERT_TRUE(parseCurrentDesktopContext(exactComponent));
    exactComponent.push_back('x');
    const auto oversizedComponent = parseCurrentDesktopContext(exactComponent);
    ASSERT_FALSE(oversizedComponent);
    EXPECT_EQ(oversizedComponent.error().code, ErrorCode::too_large);

    std::string exactEntries;
    for (std::size_t index = 0U; index < maximumCurrentDesktopEntries; ++index) {
        if (!exactEntries.empty()) {
            exactEntries.push_back(':');
        }
        exactEntries.push_back('x');
    }
    ASSERT_EQ(parseCurrentDesktopContext(exactEntries).value().names().size(),
              maximumCurrentDesktopEntries);
    exactEntries.append(":x");
    const auto tooMany = parseCurrentDesktopContext(exactEntries);
    ASSERT_FALSE(tooMany);
    EXPECT_EQ(tooMany.error().code, ErrorCode::too_large);

    std::string exactBytes;
    for (std::size_t index = 0U; index < 16U; ++index) {
        if (!exactBytes.empty()) {
            exactBytes.push_back(':');
        }
        exactBytes.append(index == 15U ? 256U : 255U, 'x');
    }
    ASSERT_EQ(exactBytes.size(), maximumCurrentDesktopBytes);
    ASSERT_TRUE(parseCurrentDesktopContext(exactBytes));
    exactBytes.push_back(':');
    const auto tooManyBytes = parseCurrentDesktopContext(exactBytes);
    ASSERT_FALSE(tooManyBytes);
    EXPECT_EQ(tooManyBytes.error().code, ErrorCode::too_large);

    constexpr std::string_view privateValue = "private-sentinel";
    const std::string withNull{"Prismdrake\0private-sentinel", 28U};
    const auto invalid = parseCurrentDesktopContext(withNull);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(invalid.error().message.find(privateValue), std::string::npos);
    EXPECT_EQ(invalid.error().recovery.find(privateValue), std::string::npos);
}

TEST(DesktopEntryVisibilityTest, HiddenAndNoDisplayTakePriority) {
    auto entry = entryWithVisibility(std::vector<std::string>{"Prismdrake"});
    entry.hidden = true;
    entry.noDisplay = true;
    EXPECT_EQ(evaluateDesktopEntryVisibility(entry, context("Prismdrake")),
              DesktopEntryVisibilityReason::hiddenTombstone);

    entry.hidden = false;
    EXPECT_EQ(evaluateDesktopEntryVisibility(entry, context("Prismdrake")),
              DesktopEntryVisibilityReason::hiddenNoDisplay);
}

TEST(DesktopEntryVisibilityTest, AbsentOnlyShowInDefaultsVisibleButPresentEmptyHides) {
    const auto absent = evaluateDesktopEntryVisibility(entryWithVisibility(), context(""));
    EXPECT_EQ(absent, DesktopEntryVisibilityReason::visibleByDefault);
    EXPECT_TRUE(isVisible(absent));

    const auto presentEmpty = evaluateDesktopEntryVisibility(
        entryWithVisibility(std::vector<std::string>{}), context("Prismdrake"));
    EXPECT_EQ(presentEmpty, DesktopEntryVisibilityReason::hiddenWithoutOnlyShowInMatch);
    EXPECT_FALSE(isVisible(presentEmpty));

    const auto emptyNotShowIn = evaluateDesktopEntryVisibility(
        entryWithVisibility(std::nullopt, std::vector<std::string>{}), context("Prismdrake"));
    EXPECT_EQ(emptyNotShowIn, DesktopEntryVisibilityReason::visibleByDefault);
}

TEST(DesktopEntryVisibilityTest, MatchesDesktopNamesExactlyAndCaseSensitively) {
    const auto only = entryWithVisibility(std::vector<std::string>{"Prismdrake"});
    EXPECT_EQ(evaluateDesktopEntryVisibility(only, context("Prismdrake")),
              DesktopEntryVisibilityReason::visibleForCurrentDesktop);
    EXPECT_EQ(evaluateDesktopEntryVisibility(only, context("prismdrake")),
              DesktopEntryVisibilityReason::hiddenWithoutOnlyShowInMatch);

    const auto excluded = entryWithVisibility(std::nullopt, std::vector<std::string>{"Prismdrake"});
    EXPECT_EQ(evaluateDesktopEntryVisibility(excluded, context("Prismdrake")),
              DesktopEntryVisibilityReason::hiddenForCurrentDesktop);
    EXPECT_EQ(evaluateDesktopEntryVisibility(excluded, context("Forge")),
              DesktopEntryVisibilityReason::visibleByDefault);
}

TEST(DesktopEntryVisibilityTest, FirstCurrentDesktopVisibilityMatchWinsDeterministically) {
    const auto entry = entryWithVisibility(std::vector<std::string>{"Prismdrake"},
                                           std::vector<std::string>{"Other"});

    EXPECT_EQ(evaluateDesktopEntryVisibility(entry, context("Prismdrake:Other")),
              DesktopEntryVisibilityReason::visibleForCurrentDesktop);
    EXPECT_EQ(evaluateDesktopEntryVisibility(entry, context("Other:Prismdrake")),
              DesktopEntryVisibilityReason::hiddenForCurrentDesktop);
}

} // namespace
} // namespace prismdrake::launcher
