#include "OutputTopology.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;

[[nodiscard]] OutputId outputId(OutputId::Value value) {
    return OutputId::fromProtocol(value).value();
}

[[nodiscard]] CrtcId crtcId(CrtcId::Value value) { return CrtcId::fromProtocol(value).value(); }

[[nodiscard]] RootGeometry rootGeometry(std::uint64_t width = 4000U, std::uint64_t height = 3000U) {
    return RootGeometry::create(width, height).value();
}

[[nodiscard]] OutputCandidate candidate(OutputId::Value output, CrtcId::Value crtc, std::int64_t x,
                                        std::int64_t y, std::uint64_t width, std::uint64_t height) {
    return {outputId(output), crtcId(crtc), x, y, width, height};
}

[[nodiscard]] OutputTopologyObservation
observation(std::vector<OutputCandidate> outputs, std::optional<OutputId> primary = std::nullopt,
            std::size_t resourceOutputCount = 0U, std::size_t resourceCrtcCount = 0U,
            std::size_t resourceModeCount = 1U, RootGeometry root = rootGeometry()) {
    if (resourceOutputCount == 0U) {
        resourceOutputCount = outputs.size();
    }
    if (resourceCrtcCount == 0U && !outputs.empty()) {
        resourceCrtcCount = outputs.size();
    }
    return {
        true,   root, resourceOutputCount, resourceCrtcCount, resourceModeCount, std::move(outputs),
        primary};
}

TEST(OutputTopologyTest, ValidatesExplicitIdentifiersRootAndScreenConversion) {
    EXPECT_FALSE(OutputId::fromProtocol(0U));
    EXPECT_FALSE(CrtcId::fromProtocol(0U));
    EXPECT_EQ(OutputId::fromProtocol(7U).value().value(), 7U);
    EXPECT_EQ(CrtcId::fromProtocol(9U).value().value(), 9U);

    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    EXPECT_TRUE(RootGeometry::create(maximum, maximum));
    EXPECT_FALSE(RootGeometry::create(0U, 1U));
    EXPECT_FALSE(RootGeometry::create(1U, 0U));
    EXPECT_FALSE(RootGeometry::create(static_cast<std::uint64_t>(maximum) + 1U, 1U));

    const auto rootWindow = WindowId::fromProtocol(1U).value();
    const ScreenInfo screen{0U, rootWindow, 1920U, 1080U};
    const auto converted = RootGeometry::fromScreenInfo(screen);
    ASSERT_TRUE(converted);
    EXPECT_EQ(converted.value().widthPx(), 1920U);
    EXPECT_EQ(converted.value().heightPx(), 1080U);
}

TEST(OutputTopologyTest, ValidatesContainedGeometryWithCheckedUint64Arithmetic) {
    const auto root = rootGeometry(100U, 100U);
    const auto exact = OutputGeometry::create(root, 99, 99, 1U, 1U);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().xPx(), 99U);
    EXPECT_EQ(exact.value().scale(), 1.0);

    EXPECT_FALSE(OutputGeometry::create(root, -1, 0, 1U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, -1, 1U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 0, 0U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 0, 1U, 0U));
    EXPECT_FALSE(OutputGeometry::create(root, 100, 0, 1U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 100, 1U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 99, 0, 2U, 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 99, 1U, 2U));
    EXPECT_FALSE(OutputGeometry::create(root, std::numeric_limits<std::int64_t>::max(), 0,
                                        std::numeric_limits<std::uint64_t>::max(), 1U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, std::numeric_limits<std::int64_t>::max(), 1U,
                                        std::numeric_limits<std::uint64_t>::max()));
}

TEST(OutputTopologyTest, SelectsAValidActivePrimaryBeforeOtherPolicies) {
    auto input = observation(
        {candidate(2U, 20U, 0, 0, 1000U, 1000U), candidate(9U, 90U, 1500, 500, 1000U, 1000U)},
        outputId(9U));

    const auto snapshot = buildOutputTopology(input);

    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot.value().selection().outputId());
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 9U);
    EXPECT_EQ(snapshot.value().selection().reason(), OutputSelectionReason::randrPrimary);
    EXPECT_EQ(snapshot.value().selection().scale(), pd1OutputScale);
}

TEST(OutputTopologyTest, RemovedPrimaryFallsBackToLowestOriginOutputIndependentOfInputOrder) {
    const auto originHigh = candidate(8U, 50U, 0, 0, 1000U, 1000U);
    const auto originLow = candidate(2U, 50U, 0, 0, 1000U, 1000U);
    auto input = observation({originHigh, candidate(4U, 40U, 1200, 0, 1000U, 1000U), originLow},
                             outputId(99U), 4U, 2U);

    auto snapshot = buildOutputTopology(input);
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot.value().selection().outputId());
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 2U);
    EXPECT_EQ(snapshot.value().selection().reason(), OutputSelectionReason::randrRootOrigin);

    std::reverse(input.activeOutputs.begin(), input.activeOutputs.end());
    snapshot = buildOutputTopology(input);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 2U);
}

TEST(OutputTopologyTest, PrimaryCloneWinsAndIdenticalCloneGeometryIsValid) {
    auto input = observation(
        {candidate(2U, 50U, 0, 0, 1000U, 1000U), candidate(8U, 50U, 0, 0, 1000U, 1000U)},
        outputId(8U), 2U, 1U);

    const auto snapshot = buildOutputTopology(input);

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().activeOutputs().size(), 2U);
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 8U);
    EXPECT_EQ(snapshot.value().selection().reason(), OutputSelectionReason::randrPrimary);
}

TEST(OutputTopologyTest, RejectsDivergentGeometryForOneCrtcWithoutPartialSnapshot) {
    auto input = observation(
        {candidate(2U, 50U, 0, 0, 1000U, 1000U), candidate(8U, 50U, 1, 0, 1000U, 1000U)},
        std::nullopt, 2U, 1U);

    const auto snapshot = buildOutputTopology(input);

    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, ErrorCode::validation_error);
    const auto startupFallback = coreRootFallbackTopology(input.coreRoot);
    EXPECT_FALSE(startupFallback.randrAvailable());
    EXPECT_TRUE(startupFallback.activeOutputs().empty());
    EXPECT_EQ(startupFallback.selection().reason(), OutputSelectionReason::coreRootFallback);
}

TEST(OutputTopologyTest, OrdersNonOriginOutputsByYThenXThenOutputIdIndependentOfInputOrder) {
    auto input = observation(
        {candidate(9U, 90U, 100, 20, 100U, 100U), candidate(7U, 70U, 200, 10, 100U, 100U),
         candidate(3U, 30U, 200, 10, 100U, 100U), candidate(1U, 10U, 300, 30, 100U, 100U)});

    auto snapshot = buildOutputTopology(input);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 3U);
    EXPECT_EQ(snapshot.value().selection().reason(), OutputSelectionReason::randrOrdered);

    std::reverse(input.activeOutputs.begin(), input.activeOutputs.end());
    snapshot = buildOutputTopology(input);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().selection().outputId()->value(), 3U);
}

TEST(OutputTopologyTest, UsesCoreRootFallbackWithoutRandrOrActiveOutputs) {
    const auto root = rootGeometry(1920U, 1080U);
    const OutputTopologyObservation unavailable{false, root, 0U, 0U, 0U, {}, std::nullopt};
    const auto noRandr = buildOutputTopology(unavailable);
    ASSERT_TRUE(noRandr);
    EXPECT_FALSE(noRandr.value().randrAvailable());
    EXPECT_FALSE(noRandr.value().selection().outputId());
    EXPECT_FALSE(noRandr.value().selection().crtcId());
    EXPECT_EQ(noRandr.value().selection().geometry().widthPx(), 1920U);
    EXPECT_EQ(noRandr.value().selection().geometry().heightPx(), 1080U);
    EXPECT_EQ(noRandr.value().selection().scale(), 1.0);

    const auto noActive =
        buildOutputTopology(OutputTopologyObservation{true, root, 3U, 2U, 4U, {}, outputId(99U)});
    ASSERT_TRUE(noActive);
    EXPECT_TRUE(noActive.value().randrAvailable());
    EXPECT_EQ(noActive.value().selection().reason(), OutputSelectionReason::coreRootFallback);
}

TEST(OutputTopologyTest, RejectsUnavailableRandrWithAnyStaleRandrFields) {
    const auto root = rootGeometry();
    const auto stale = buildOutputTopology(OutputTopologyObservation{
        false, root, 1U, 1U, 1U, {candidate(1U, 1U, 0, 0, 100U, 100U)}, outputId(1U)});
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::validation_error);
}

TEST(OutputTopologyTest, RejectsDuplicateOutputsAndInconsistentResourceCounts) {
    const auto duplicate = buildOutputTopology(
        observation({candidate(1U, 1U, 0, 0, 100U, 100U), candidate(1U, 2U, 200, 0, 100U, 100U)}));
    EXPECT_FALSE(duplicate);

    const auto tooFewOutputs = buildOutputTopology(
        observation({candidate(1U, 1U, 0, 0, 100U, 100U), candidate(2U, 2U, 200, 0, 100U, 100U)},
                    std::nullopt, 1U, 2U));
    EXPECT_FALSE(tooFewOutputs);

    const auto tooFewCrtcs = buildOutputTopology(
        observation({candidate(1U, 1U, 0, 0, 100U, 100U), candidate(2U, 2U, 200, 0, 100U, 100U)},
                    std::nullopt, 2U, 1U));
    EXPECT_FALSE(tooFewCrtcs);

    const auto noModes = buildOutputTopology(
        observation({candidate(1U, 1U, 0, 0, 100U, 100U)}, std::nullopt, 1U, 1U, 0U));
    EXPECT_FALSE(noModes);
}

TEST(OutputTopologyTest, EnforcesOutputCrtcModeAndReplyPolicyBounds) {
    EXPECT_EQ(maximumTopologyOutputs, 64U);
    EXPECT_EQ(maximumTopologyCrtcs, 64U);
    EXPECT_EQ(maximumTopologyModes, 4096U);
    EXPECT_EQ(maximumTopologyReplyBytes, 1024U * 1024U);

    const auto root = rootGeometry();
    const auto outputs = buildOutputTopology(OutputTopologyObservation{
        true, root, maximumTopologyOutputs + 1U, 0U, 0U, {}, std::nullopt});
    ASSERT_FALSE(outputs);
    EXPECT_EQ(outputs.error().code, ErrorCode::too_large);

    const auto crtcs = buildOutputTopology(
        OutputTopologyObservation{true, root, 0U, maximumTopologyCrtcs + 1U, 0U, {}, std::nullopt});
    ASSERT_FALSE(crtcs);
    EXPECT_EQ(crtcs.error().code, ErrorCode::too_large);

    const auto modes = buildOutputTopology(
        OutputTopologyObservation{true, root, 0U, 0U, maximumTopologyModes + 1U, {}, std::nullopt});
    ASSERT_FALSE(modes);
    EXPECT_EQ(modes.error().code, ErrorCode::too_large);

    std::vector<OutputCandidate> tooManyActive;
    for (std::uint32_t index = 1U; index <= maximumTopologyOutputs + 1U; ++index) {
        tooManyActive.push_back(candidate(index, 1U, 0, 0, 1U, 1U));
    }
    const auto active = buildOutputTopology(OutputTopologyObservation{
        true, root, maximumTopologyOutputs, 1U, 1U, std::move(tooManyActive), std::nullopt});
    ASSERT_FALSE(active);
    EXPECT_EQ(active.error().code, ErrorCode::too_large);
}

TEST(OutputTopologyTest, RejectsEveryInvalidCandidateWithoutPublishingValidSiblings) {
    const auto root = rootGeometry(100U, 100U);
    const auto valid = candidate(1U, 1U, 0, 0, 10U, 10U);
    const std::vector invalidCandidates{
        candidate(2U, 2U, -1, 0, 1U, 1U),
        candidate(2U, 2U, 0, -1, 1U, 1U),
        candidate(2U, 2U, 0, 0, 0U, 1U),
        candidate(2U, 2U, 0, 0, 1U, 0U),
        candidate(2U, 2U, 99, 0, 2U, 1U),
        candidate(2U, 2U, 0, 99, 1U, 2U),
        candidate(2U, 2U, std::numeric_limits<std::int64_t>::max(), 0,
                  std::numeric_limits<std::uint64_t>::max(), 1U),
    };
    for (const auto &invalid : invalidCandidates) {
        const auto snapshot =
            buildOutputTopology(observation({valid, invalid}, std::nullopt, 2U, 2U, 1U, root));
        EXPECT_FALSE(snapshot);
    }
}

} // namespace
} // namespace prismdrake::x11
