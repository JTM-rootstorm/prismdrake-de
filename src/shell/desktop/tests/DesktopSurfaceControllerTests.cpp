#include "DesktopSurfaceController.hpp"

#include "TestTopology.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace prismdrake::shell::desktop {
namespace {

TEST(DesktopSurfaceControllerTest, BuildsOneScaledSurfacePerCompleteRegistryPlacement) {
    test::RegistryHarness registry;
    const auto placements = registry.observeMixedScale(1U);
    const auto wallpaper = test::imageWallpaper(7U, config::WallpaperMode::center);
    const std::array scales{OutputScaleObservation{"HDMI-1", 1.0},
                            OutputScaleObservation{"DP-1", 2.0}};
    DesktopSurfaceController controller;

    auto update = controller.prepare(placements, scales, wallpaper);

    ASSERT_TRUE(update) << update.error().message;
    ASSERT_EQ(update.value().snapshot()->surfaces().size(), 2U);
    EXPECT_EQ(update.value().snapshot()->wallpaper(), wallpaper);
    EXPECT_EQ(update.value().snapshot()->topologyGeneration(), 1U);
    for (const auto &surface : update.value().snapshot()->surfaces()) {
        EXPECT_TRUE(surface.wallpaperLayout.has_value());
    }
    const auto dp = std::find_if(update.value().snapshot()->surfaces().begin(),
                                 update.value().snapshot()->surfaces().end(),
                                 [](const auto &surface) { return surface.outputName == "DP-1"; });
    ASSERT_NE(dp, update.value().snapshot()->surfaces().end());
    EXPECT_EQ(dp->logicalGeometry, QRect(-1280, 0, 1280, 720));
    EXPECT_DOUBLE_EQ(dp->devicePixelRatio, 2.0);
    ASSERT_TRUE(dp->wallpaperLayout);
    EXPECT_EQ(dp->wallpaperLayout->targetRect.size(), QSizeF(32.0, 16.0));
}

TEST(DesktopSurfaceControllerTest, RejectsIncompleteScaleAndStaleWallpaperAtomically) {
    test::RegistryHarness registry;
    const auto placements = registry.observe(1U);
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    DesktopSurfaceController controller;
    auto initial = controller.prepare(placements, scales, test::solidWallpaper(3U));
    ASSERT_TRUE(initial);
    controller.commit(std::move(initial).value());
    const auto retained = controller.current();

    const std::array missingScale{OutputScaleObservation{"LEFT-1", 1.0}};
    EXPECT_FALSE(controller.prepare(placements, missingScale, test::solidWallpaper(4U)));
    const std::array invalidScale{
        OutputScaleObservation{"LEFT-1", std::numeric_limits<double>::infinity()},
        OutputScaleObservation{"RIGHT-1", 1.0}};
    EXPECT_FALSE(controller.prepare(placements, invalidScale, test::solidWallpaper(4U)));
    EXPECT_FALSE(controller.prepare(placements, scales, test::solidWallpaper(2U)));
    EXPECT_EQ(controller.current(), retained);
}

TEST(DesktopSurfaceControllerTest, AcceptsIdempotentTopologyAndRejectsEqualGenerationConflicts) {
    test::RegistryHarness initialRegistry;
    test::RegistryHarness duplicateRegistry;
    test::RegistryHarness geometryConflictRegistry;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    DesktopSurfaceController controller;
    const auto initialWallpaper = test::solidWallpaper(3U);
    auto initial = controller.prepare(initialRegistry.observe(7U), scales, initialWallpaper);
    ASSERT_TRUE(initial);
    controller.commit(std::move(initial).value());
    const auto retained = controller.current();

    const auto duplicatePlacements = duplicateRegistry.observe(7U);
    auto duplicate = controller.prepare(duplicatePlacements, scales, initialWallpaper);
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_EQ(duplicate.value().snapshot()->topologyGeneration(), 7U);

    const std::array conflictingScales{OutputScaleObservation{"LEFT-1", 1.25},
                                       OutputScaleObservation{"RIGHT-1", 1.0}};
    EXPECT_FALSE(
        controller.prepare(duplicatePlacements, conflictingScales, test::solidWallpaper(4U)));

    const auto conflictingGeometry =
        geometryConflictRegistry.observe(7U, true, outputs::LogicalRect{12, 0, 500U, 768U});
    EXPECT_FALSE(controller.prepare(conflictingGeometry, scales, test::solidWallpaper(4U)));
    EXPECT_EQ(controller.current(), retained);

    auto wallpaperOnly = controller.prepare(duplicatePlacements, scales, test::solidWallpaper(4U));
    ASSERT_TRUE(wallpaperOnly) << wallpaperOnly.error().message;
    controller.commit(std::move(wallpaperOnly).value());
    EXPECT_EQ(controller.current()->topologyGeneration(), 7U);
    EXPECT_EQ(controller.current()->wallpaper()->generation, 4U);
}

TEST(DesktopSurfaceControllerTest, RetainsSafeWallpaperAcrossSettingsLossAndOutputRemoval) {
    test::RegistryHarness registry;
    const auto firstPlacements = registry.observe(1U);
    const std::array twoScales{OutputScaleObservation{"LEFT-1", 1.0},
                               OutputScaleObservation{"RIGHT-1", 1.0}};
    DesktopSurfaceController controller;
    auto first = controller.prepare(firstPlacements, twoScales, test::solidWallpaper(9U));
    ASSERT_TRUE(first);
    controller.commit(std::move(first).value());
    const auto retainedWallpaper = controller.current()->wallpaper();
    EXPECT_TRUE(controller.settingsOwnerAvailable());

    controller.settingsOwnerLost();
    EXPECT_FALSE(controller.settingsOwnerAvailable());
    const auto secondPlacements = registry.observe(2U, false);
    const std::array oneScale{OutputScaleObservation{"LEFT-1", 1.0}};
    auto second = controller.prepare(secondPlacements, oneScale);
    ASSERT_TRUE(second) << second.error().message;
    controller.commit(std::move(second).value());

    ASSERT_EQ(controller.current()->surfaces().size(), 1U);
    EXPECT_EQ(controller.current()->wallpaper(), retainedWallpaper);
    EXPECT_EQ(controller.current()->wallpaper()->generation, 9U);
    EXPECT_FALSE(controller.settingsOwnerAvailable());

    auto recovered = controller.prepare(secondPlacements, oneScale, test::solidWallpaper(1U));
    ASSERT_TRUE(recovered);
    controller.commit(std::move(recovered).value());
    EXPECT_TRUE(controller.settingsOwnerAvailable());
    EXPECT_EQ(controller.current()->wallpaper()->generation, 1U);
}

} // namespace
} // namespace prismdrake::shell::desktop
