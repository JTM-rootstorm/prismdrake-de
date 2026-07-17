#include "EwmhTaskList.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;

[[nodiscard]] std::vector<std::uint32_t> values(std::span<const WindowId> windows) {
    std::vector<std::uint32_t> result;
    result.reserve(windows.size());
    for (const auto window : windows) {
        result.push_back(window.value());
    }
    return result;
}

[[nodiscard]] EwmhTaskListObservation
observation(std::vector<std::uint32_t> clients,
            std::optional<std::vector<std::uint32_t>> stacking = std::nullopt,
            std::optional<std::uint32_t> active = std::nullopt) {
    return {std::move(clients), std::move(stacking), active};
}

TEST(EwmhTaskListTest, PreservesAuthoritativeClientAndReorderedStackingLists) {
    const auto snapshot = buildEwmhTaskListSnapshot(
        observation({40U, 10U, 30U}, std::vector<std::uint32_t>{30U, 40U, 10U}, 40U));

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(values(snapshot.value().clientList()), (std::vector<std::uint32_t>{40U, 10U, 30U}));
    EXPECT_EQ(values(snapshot.value().stackingOrder()),
              (std::vector<std::uint32_t>{30U, 40U, 10U}));
    ASSERT_TRUE(snapshot.value().activeWindow());
    EXPECT_EQ(snapshot.value().activeWindow()->value(), 40U);
    EXPECT_EQ(snapshot.value().stackingSource(), EwmhStackingSource::clientListStacking);
    EXPECT_TRUE(snapshot.value().contains(WindowId::fromProtocol(10U).value()));
    EXPECT_FALSE(snapshot.value().contains(WindowId::fromProtocol(99U).value()));
}

TEST(EwmhTaskListTest, MissingOptionalPropertiesUseDeterministicClientListFallback) {
    const auto first = buildEwmhTaskListSnapshot(observation({8U, 2U, 5U}));
    ASSERT_TRUE(first);
    EXPECT_EQ(values(first.value().stackingOrder()), (std::vector<std::uint32_t>{8U, 2U, 5U}));
    EXPECT_FALSE(first.value().activeWindow());
    EXPECT_EQ(first.value().stackingSource(), EwmhStackingSource::clientListFallback);

    const auto reordered = buildEwmhTaskListSnapshot(observation({5U, 8U, 2U}));
    ASSERT_TRUE(reordered);
    EXPECT_EQ(values(reordered.value().clientList()), (std::vector<std::uint32_t>{5U, 8U, 2U}));
    EXPECT_EQ(values(reordered.value().stackingOrder()), (std::vector<std::uint32_t>{5U, 8U, 2U}));
}

TEST(EwmhTaskListTest, AcceptsPresentEmptyListsWithoutInventingTasks) {
    const auto fallback = buildEwmhTaskListSnapshot(observation({}));
    ASSERT_TRUE(fallback);
    EXPECT_TRUE(fallback.value().clientList().empty());
    EXPECT_TRUE(fallback.value().stackingOrder().empty());
    EXPECT_FALSE(fallback.value().activeWindow());

    const auto explicitStacking =
        buildEwmhTaskListSnapshot(observation({}, std::vector<std::uint32_t>{}));
    ASSERT_TRUE(explicitStacking);
    EXPECT_EQ(explicitStacking.value().stackingSource(), EwmhStackingSource::clientListStacking);
}

TEST(EwmhTaskListTest, RejectsMissingRequiredClientListWithoutPartialState) {
    const auto snapshot = buildEwmhTaskListSnapshot(
        EwmhTaskListObservation{std::nullopt, std::vector<std::uint32_t>{7U}, 7U});

    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, ErrorCode::validation_error);
}

TEST(EwmhTaskListTest, RejectsZeroAndDuplicateIdentifiersInEitherList) {
    const std::array malformed{
        observation({0U}),
        observation({1U, 1U}),
        observation({1U, 2U}, std::vector<std::uint32_t>{0U, 2U}),
        observation({1U, 2U}, std::vector<std::uint32_t>{1U, 1U}),
    };

    for (const auto &item : malformed) {
        const auto snapshot = buildEwmhTaskListSnapshot(item);
        ASSERT_FALSE(snapshot);
        EXPECT_EQ(snapshot.error().code, ErrorCode::validation_error);
    }
}

TEST(EwmhTaskListTest, RejectsStackingListsThatAreNotExactClientPermutations) {
    const std::array malformed{
        observation({1U, 2U}, std::vector<std::uint32_t>{1U}),
        observation({1U, 2U}, std::vector<std::uint32_t>{1U, 3U}),
        observation({1U}, std::vector<std::uint32_t>{1U, 2U}),
    };

    for (const auto &item : malformed) {
        const auto snapshot = buildEwmhTaskListSnapshot(item);
        ASSERT_FALSE(snapshot);
        EXPECT_EQ(snapshot.error().code, ErrorCode::validation_error);
    }
}

TEST(EwmhTaskListTest, RejectsZeroOrNonmemberActiveWindows) {
    const auto zero = buildEwmhTaskListSnapshot(observation({1U, 2U}, std::nullopt, 0U));
    ASSERT_FALSE(zero);
    EXPECT_EQ(zero.error().code, ErrorCode::validation_error);

    const auto nonmember = buildEwmhTaskListSnapshot(observation({1U, 2U}, std::nullopt, 3U));
    ASSERT_FALSE(nonmember);
    EXPECT_EQ(nonmember.error().code, ErrorCode::validation_error);

    const auto activeWithNoClients = buildEwmhTaskListSnapshot(observation({}, std::nullopt, 1U));
    ASSERT_FALSE(activeWithNoClients);
    EXPECT_EQ(activeWithNoClients.error().code, ErrorCode::validation_error);
}

TEST(EwmhTaskListTest, EnforcesStrictIndependentListBounds) {
    EXPECT_EQ(maximumEwmhTaskWindows, 256U);
    std::vector<std::uint32_t> maximum;
    maximum.reserve(maximumEwmhTaskWindows);
    for (std::uint32_t value = 1U; value <= maximumEwmhTaskWindows; ++value) {
        maximum.push_back(value);
    }
    auto reversed = maximum;
    std::reverse(reversed.begin(), reversed.end());
    const auto accepted = buildEwmhTaskListSnapshot(observation(maximum, std::move(reversed), 1U));
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().clientList().size(), maximumEwmhTaskWindows);

    maximum.push_back(static_cast<std::uint32_t>(maximumEwmhTaskWindows + 1U));
    const auto oversizedClients = buildEwmhTaskListSnapshot(observation(maximum));
    ASSERT_FALSE(oversizedClients);
    EXPECT_EQ(oversizedClients.error().code, ErrorCode::too_large);

    const auto oversizedStacking = buildEwmhTaskListSnapshot(
        observation({1U}, std::optional<std::vector<std::uint32_t>>{maximum}));
    ASSERT_FALSE(oversizedStacking);
    EXPECT_EQ(oversizedStacking.error().code, ErrorCode::too_large);
}

} // namespace
} // namespace prismdrake::x11
