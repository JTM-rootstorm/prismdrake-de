#include "Pd2ShellRuntime.hpp"

#include <gtest/gtest.h>

#include <QColor>

#include <memory>

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot>
wallpaper(std::uint64_t generation, const char *color) {
    return std::make_shared<const prismdrake::desktop::WallpaperSnapshot>(
        prismdrake::desktop::WallpaperSnapshot{generation,
                                               QColor{color},
                                               config::WallpaperMode::solid,
                                               prismdrake::desktop::WallpaperLoadStatus::solid,
                                               {},
                                               {}});
}

TEST(SettingsWallpaperEpochGateTest, BlocksPriorWallpaperWhileReplacementDecodeIsPending) {
    SettingsWallpaperEpochGate gate;
    const auto committed = wallpaper(5U, "#182033");
    ASSERT_TRUE(gate.publish(1U, 1U, 5U, committed));
    EXPECT_EQ(gate.current(1U, 5U), committed);

    // The mapped desktop owns the committed snapshot. Clearing only the runtime candidate leaves
    // that visible surface intact while the settings owner is absent.
    gate.ownerLost();
    EXPECT_FALSE(gate.current(1U, 5U));

    // A topology/panel publication during the blocked replacement decode has no eligible
    // candidate and therefore cannot reapply generation 5 under replacement owner epoch 2.
    EXPECT_FALSE(gate.current(2U, 1U));
    EXPECT_FALSE(gate.publish(1U, 2U, 1U, committed));
    EXPECT_FALSE(gate.current(2U, 1U));

    const auto replacement = wallpaper(1U, "#334455");
    ASSERT_TRUE(gate.publish(2U, 2U, 1U, replacement));
    EXPECT_EQ(gate.current(2U, 1U), replacement);
}

TEST(SettingsWallpaperEpochGateTest, RejectsCandidatesOutsideActiveGeneration) {
    SettingsWallpaperEpochGate gate;
    EXPECT_FALSE(gate.publish(0U, 0U, 1U, wallpaper(1U, "#182033")));
    EXPECT_FALSE(gate.publish(3U, 3U, 0U, wallpaper(1U, "#182033")));
    EXPECT_FALSE(gate.publish(3U, 3U, 4U, wallpaper(3U, "#182033")));
    EXPECT_FALSE(gate.publish(3U, 3U, 4U, {}));
}

} // namespace
} // namespace prismdrake::shell::runtime
