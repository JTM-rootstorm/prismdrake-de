#include "WallpaperModel.hpp"

#include <gtest/gtest.h>

namespace prismdrake::desktop {
namespace {

TEST(WallpaperModelTest, ConvertsFrozenDesktopSettingsWithExplicitRelativeBase) {
    const config::Desktop settings{"wallpapers/original.png", "#182033", config::WallpaperMode::fit,
                                   false};
    const auto result = makeWallpaperRequest(settings, "/tmp/config", 42U);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().source, "wallpapers/original.png");
    EXPECT_EQ(result.value().sourceBaseDirectory, "/tmp/config");
    EXPECT_EQ(result.value().generation, 42U);
    EXPECT_EQ(result.value().mode, config::WallpaperMode::fit);
}

TEST(WallpaperModelTest, CountsUnicodeScalarsRatherThanUtf16CodeUnits) {
    const QString dragon = QString::fromUtf8("\xF0\x9F\x90\x89");
    const QString bounded = dragon.repeated(128);
    const config::Desktop accepted{bounded.toUtf8().toStdString(), "#182033",
                                   config::WallpaperMode::fit, false};
    EXPECT_TRUE(makeWallpaperRequest(accepted, "/tmp/config", 1U));

    const config::Desktop oversized{(bounded + dragon).toUtf8().toStdString(), "#182033",
                                    config::WallpaperMode::fit, false};
    EXPECT_FALSE(makeWallpaperRequest(oversized, "/tmp/config", 2U));
}

TEST(WallpaperModelTest, RejectsSyntheticInvalidModeAndBaseUnicode) {
    const config::Desktop invalidMode{"", "#182033", static_cast<config::WallpaperMode>(255),
                                      false};
    EXPECT_FALSE(makeWallpaperRequest(invalidMode, "/tmp/config", 1U));

    QString unpaired;
    unpaired.append(QChar{0xd800});
    const config::Desktop valid{"", "#182033", config::WallpaperMode::solid, false};
    EXPECT_FALSE(makeWallpaperRequest(valid, unpaired, 2U));
}

} // namespace
} // namespace prismdrake::desktop
