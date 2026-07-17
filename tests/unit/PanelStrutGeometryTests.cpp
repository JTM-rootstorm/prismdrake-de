#include "PanelStrutGeometry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] RootGeometry rootGeometry(std::uint64_t width, std::uint64_t height) {
    return RootGeometry::create(width, height).value();
}

[[nodiscard]] OutputGeometry outputGeometry(RootGeometry root, std::int64_t x, std::int64_t y,
                                            std::uint64_t width, std::uint64_t height) {
    return OutputGeometry::create(root, x, y, width, height).value();
}

TEST(PanelStrutGeometryTest, ReservesRootEdgeForRootWidthOutput) {
    const auto root = rootGeometry(1920U, 1080U);
    const auto calculated =
        calculateBottomPanelStrut(root, outputGeometry(root, 0, 0, 1920U, 1080U), 48U);
    ASSERT_TRUE(calculated);
    EXPECT_EQ(calculated.value().panel, (BottomPanelGeometry{0U, 1032U, 1920U, 48U}));
    EXPECT_EQ(calculated.value().strut, (std::array<std::uint32_t, 4U>{0U, 0U, 0U, 48U}));
    EXPECT_EQ(calculated.value().strutPartial,
              (std::array<std::uint32_t, 12U>{0U, 0U, 0U, 48U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1919U}));
}

TEST(PanelStrutGeometryTest, MatchesEwmhUnequalHeightOutputExample) {
    const auto root = rootGeometry(2304U, 1024U);
    const auto calculated =
        calculateBottomPanelStrut(root, outputGeometry(root, 1280, 0, 1024U, 768U), 50U);
    ASSERT_TRUE(calculated);
    EXPECT_EQ(calculated.value().panel, (BottomPanelGeometry{1280U, 718U, 1024U, 50U}));
    EXPECT_EQ(calculated.value().strut, (std::array<std::uint32_t, 4U>{0U, 0U, 0U, 306U}));
    EXPECT_EQ(
        calculated.value().strutPartial,
        (std::array<std::uint32_t, 12U>{0U, 0U, 0U, 306U, 0U, 0U, 0U, 0U, 0U, 0U, 1280U, 2303U}));
}

TEST(PanelStrutGeometryTest, UsesRootCoordinatesForInsetOutput) {
    const auto root = rootGeometry(1000U, 800U);
    const auto calculated =
        calculateBottomPanelStrut(root, outputGeometry(root, 100, 100, 800U, 600U), 40U);
    ASSERT_TRUE(calculated);
    EXPECT_EQ(calculated.value().panel, (BottomPanelGeometry{100U, 660U, 800U, 40U}));
    EXPECT_EQ(calculated.value().strut, (std::array<std::uint32_t, 4U>{0U, 0U, 0U, 140U}));
    EXPECT_EQ(
        calculated.value().strutPartial,
        (std::array<std::uint32_t, 12U>{0U, 0U, 0U, 140U, 0U, 0U, 0U, 0U, 0U, 0U, 100U, 899U}));
}

TEST(PanelStrutGeometryTest, AcceptsPanelHeightEqualToOutputHeight) {
    const auto root = rootGeometry(800U, 600U);
    const auto calculated =
        calculateBottomPanelStrut(root, outputGeometry(root, 100, 200, 300U, 250U), 250U);
    ASSERT_TRUE(calculated);
    EXPECT_EQ(calculated.value().panel, (BottomPanelGeometry{100U, 200U, 300U, 250U}));
    EXPECT_EQ(calculated.value().strut[3], 400U);
}

TEST(PanelStrutGeometryTest, RejectsZeroDimensionsAndInvalidPanelHeights) {
    EXPECT_FALSE(RootGeometry::create(0U, 600U));
    EXPECT_FALSE(RootGeometry::create(800U, 0U));
    const auto root = rootGeometry(800U, 600U);
    EXPECT_FALSE(OutputGeometry::create(root, 0, 0, 0U, 600U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 0, 800U, 0U));
    const auto output = outputGeometry(root, 0, 0, 800U, 600U);
    EXPECT_FALSE(calculateBottomPanelStrut(root, output, 0U));
    EXPECT_FALSE(calculateBottomPanelStrut(root, output, 601U));
}

TEST(PanelStrutGeometryTest, RejectsNegativeOrUncontainedOutputGeometry) {
    const auto root = rootGeometry(800U, 600U);
    EXPECT_FALSE(OutputGeometry::create(root, -1, 0, 100U, 100U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, -1, 100U, 100U));
    EXPECT_FALSE(OutputGeometry::create(root, 701, 0, 100U, 100U));
    EXPECT_FALSE(OutputGeometry::create(root, 0, 501, 100U, 100U));

    const auto largerRoot = rootGeometry(1000U, 600U);
    const auto outputFromLargerRoot = outputGeometry(largerRoot, 900, 0, 100U, 100U);
    EXPECT_FALSE(calculateBottomPanelStrut(root, outputFromLargerRoot, 20U));
}

TEST(PanelStrutGeometryTest, RejectsCoordinateArithmeticOverflowWithoutAValue) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto maximumRoot = rootGeometry(maximum, maximum);

    EXPECT_FALSE(OutputGeometry::create(maximumRoot, std::numeric_limits<std::int64_t>::max(), 0,
                                        maximum, 1U));
    EXPECT_FALSE(OutputGeometry::create(maximumRoot, 0, std::numeric_limits<std::int64_t>::max(),
                                        1U, maximum));
}

TEST(PanelStrutGeometryTest, AcceptsMaximumContainedCardinalBoundary) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto root = rootGeometry(maximum, maximum);
    const auto calculated =
        calculateBottomPanelStrut(root, outputGeometry(root, 0, 0, maximum, maximum), 1U);
    ASSERT_TRUE(calculated);
    EXPECT_EQ(calculated.value().panel.y, maximum - 1U);
    EXPECT_EQ(calculated.value().strut[3], 1U);
    EXPECT_EQ(calculated.value().strutPartial[11], maximum - 1U);
}

TEST(PanelStrutGeometryTest, SupportsCoordinatesBeyondInt32AndOnePixelInclusiveRange) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t largeX =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U;
    const auto root = rootGeometry(maximum, maximum);
    const auto largeOutput =
        outputGeometry(root, static_cast<std::int64_t>(largeX), 0, maximum - largeX, maximum);
    const auto large = calculateBottomPanelStrut(root, largeOutput, 1U);
    ASSERT_TRUE(large);
    EXPECT_EQ(large.value().strutPartial[10], largeX);
    EXPECT_EQ(large.value().strutPartial[11], maximum - 1U);

    const auto tinyRoot = rootGeometry(100U, 100U);
    const auto onePixel =
        calculateBottomPanelStrut(tinyRoot, outputGeometry(tinyRoot, 99, 99, 1U, 1U), 1U);
    ASSERT_TRUE(onePixel);
    EXPECT_EQ(onePixel.value().strutPartial[10], 99U);
    EXPECT_EQ(onePixel.value().strutPartial[11], 99U);
}

} // namespace
} // namespace prismdrake::x11
