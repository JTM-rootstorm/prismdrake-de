#include "BuildInfo.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace prismdrake::foundation {
namespace {

TEST(BuildInfoTest, ReportsTrackedDevelopmentVersion) {
    EXPECT_EQ(productVersion(), std::string_view{PRISMDRAKE_TEST_VERSION});
}

TEST(BuildInfoTest, ReportsConfiguredDeveloperOverrideState) {
    EXPECT_EQ(developerOverridesEnabled(), PRISMDRAKE_TEST_DEVELOPER_OVERRIDES != 0);
}

} // namespace
} // namespace prismdrake::foundation
