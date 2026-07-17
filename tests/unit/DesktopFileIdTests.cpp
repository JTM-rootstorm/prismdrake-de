#include "DesktopFileId.hpp"

#include <gtest/gtest.h>

#include <string>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;

TEST(DesktopFileIdTest, DerivesSpecificationIdentifierWithoutChangingCaseOrExtension) {
    const auto nested = deriveDesktopFileId("vendor/tools/PrismDrake.desktop");
    ASSERT_TRUE(nested);
    EXPECT_EQ(nested.value().value(), "vendor-tools-PrismDrake.desktop");

    const auto topLevel = deriveDesktopFileId("PrismDrake.desktop");
    ASSERT_TRUE(topLevel);
    EXPECT_EQ(topLevel.value().value(), "PrismDrake.desktop");
    EXPECT_EQ(topLevel.value(), topLevel.value());
    EXPECT_NE(topLevel.value(), nested.value());
}

TEST(DesktopFileIdTest, RejectsRootEmptyTraversalAndEmptyComponents) {
    for (const auto *path :
         {"", "/tool.desktop", "tool.desktop/", "a//tool.desktop", "./tool.desktop",
          "a/./tool.desktop", "../tool.desktop", "a/../tool.desktop"}) {
        const auto result = deriveDesktopFileId(path);
        ASSERT_FALSE(result) << path;
        EXPECT_EQ(result.error().code, ErrorCode::invalid_argument) << path;
    }
}

TEST(DesktopFileIdTest, RequiresExactDesktopExtensionAndNonemptyStem) {
    for (const auto *path :
         {"tool", "tool.Desktop", "tool.desktop.bak", ".desktop", "nested/.desktop"}) {
        const auto result = deriveDesktopFileId(path);
        ASSERT_FALSE(result) << path;
        EXPECT_EQ(result.error().code, ErrorCode::validation_error) << path;
    }
}

TEST(DesktopFileIdTest, AcceptsExactPathDepthAndIdentifierBounds) {
    std::string exactDepth;
    for (std::size_t index = 1U; index < maximumDesktopFilePathComponents; ++index) {
        exactDepth.append("a/");
    }
    exactDepth.append("tool.desktop");
    ASSERT_TRUE(deriveDesktopFileId(exactDepth));

    std::string tooDeep = "a/";
    tooDeep.append(exactDepth);
    const auto depthResult = deriveDesktopFileId(tooDeep);
    ASSERT_FALSE(depthResult);
    EXPECT_EQ(depthResult.error().code, ErrorCode::too_large);

    std::string exactBytes(maximumDesktopFileRelativePathBytes - 8U, 'x');
    exactBytes.append(".desktop");
    ASSERT_EQ(exactBytes.size(), maximumDesktopFileRelativePathBytes);
    const auto exact = deriveDesktopFileId(exactBytes);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().value().size(), maximumDesktopFileIdBytes);

    exactBytes.insert(exactBytes.begin(), 'x');
    const auto oversized = deriveDesktopFileId(exactBytes);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::too_large);
}

TEST(DesktopFileIdTest, RejectsNullWithoutDisclosingInput) {
    constexpr std::string_view privateValue = "private-sentinel";
    const std::string path{"tool\0private-sentinel.desktop", 29U};

    const auto result = deriveDesktopFileId(path);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(result.error().message.find(privateValue), std::string::npos);
    EXPECT_EQ(result.error().recovery.find(privateValue), std::string::npos);
}

} // namespace
} // namespace prismdrake::launcher
