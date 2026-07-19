#include "WallpaperLayout.hpp"

#include <gtest/gtest.h>

namespace prismdrake::desktop {
namespace {

TEST(WallpaperLayoutTest, CenterPreservesPixelsAndUsesDeterministicOrigin) {
    const auto layout = calculateWallpaperLayout(config::WallpaperMode::center, QSize{800, 600},
                                                 QSizeF{1920.0, 1080.0});
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout->sourceRect, QRectF(0.0, 0.0, 800.0, 600.0));
    EXPECT_EQ(layout->targetRect, QRectF(560.0, 240.0, 800.0, 600.0));
    EXPECT_FALSE(layout->tiled);
}

TEST(WallpaperLayoutTest, FitLetterboxesAndFillCropsSymmetrically) {
    const auto fit = calculateWallpaperLayout(config::WallpaperMode::fit, QSize{400, 300},
                                              QSizeF{1920.0, 1080.0});
    ASSERT_TRUE(fit);
    EXPECT_EQ(fit->sourceRect, QRectF(0.0, 0.0, 400.0, 300.0));
    EXPECT_EQ(fit->targetRect, QRectF(240.0, 0.0, 1440.0, 1080.0));

    const auto fill = calculateWallpaperLayout(config::WallpaperMode::fill, QSize{400, 300},
                                               QSizeF{1920.0, 1080.0});
    ASSERT_TRUE(fill);
    EXPECT_EQ(fill->sourceRect, QRectF(0.0, 37.5, 400.0, 225.0));
    EXPECT_EQ(fill->targetRect, QRectF(0.0, 0.0, 1920.0, 1080.0));
}

TEST(WallpaperLayoutTest, StretchAndTileHaveBoundedRepresentations) {
    const auto stretch =
        calculateWallpaperLayout(config::WallpaperMode::stretch, QSize{4, 3}, QSizeF{200.0, 100.0});
    ASSERT_TRUE(stretch);
    EXPECT_EQ(stretch->targetRect, QRectF(0.0, 0.0, 200.0, 100.0));
    EXPECT_FALSE(stretch->tiled);

    const auto tile = calculateWallpaperLayout(config::WallpaperMode::tile, QSize{4, 3},
                                               QSizeF{1'000'000.0, 1'000'000.0});
    ASSERT_TRUE(tile);
    EXPECT_TRUE(tile->tiled);
    EXPECT_EQ(tile->tileSize, QSizeF(4.0, 3.0));
    EXPECT_EQ(tile->tileOrigin, QPointF(0.0, 0.0));
}

TEST(WallpaperLayoutTest, RejectsSolidAndInvalidGeometry) {
    EXPECT_FALSE(
        calculateWallpaperLayout(config::WallpaperMode::solid, QSize{1, 1}, QSizeF{100.0, 100.0}));
    EXPECT_FALSE(
        calculateWallpaperLayout(config::WallpaperMode::fit, QSize{}, QSizeF{100.0, 100.0}));
    EXPECT_FALSE(calculateWallpaperLayout(config::WallpaperMode::fit, QSize{1, 1}, QSizeF{}));
    EXPECT_FALSE(
        calculateWallpaperLayout(config::WallpaperMode::fit, QSize{1, 1},
                                 QSizeF{maximumWallpaperOutputLogicalExtent + 1.0, 100.0}));
    EXPECT_FALSE(calculateWallpaperLayout(config::WallpaperMode::fit, QSize{1, 1},
                                          QSizeF{100.0, 100.0}, 9.0));
    EXPECT_FALSE(calculateWallpaperLayout(static_cast<config::WallpaperMode>(255), QSize{1, 1},
                                          QSizeF{100.0, 100.0}));
}

TEST(WallpaperLayoutTest, CenterAndTileMapSourcePixelsToDevicePixels) {
    const auto center = calculateWallpaperLayout(config::WallpaperMode::center, QSize{800, 600},
                                                 QSizeF{1000.0, 500.0}, 2.0);
    ASSERT_TRUE(center);
    EXPECT_EQ(center->targetRect, QRectF(300.0, 100.0, 400.0, 300.0));

    const auto tile = calculateWallpaperLayout(config::WallpaperMode::tile, QSize{80, 60},
                                               QSizeF{1000.0, 500.0}, 1.25);
    ASSERT_TRUE(tile);
    EXPECT_EQ(tile->tileSize, QSizeF(64.0, 48.0));
}

} // namespace
} // namespace prismdrake::desktop
