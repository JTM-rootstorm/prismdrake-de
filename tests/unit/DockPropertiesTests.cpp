#include "DockPropertiesInternal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] AtomId atom(std::uint32_t value) { return AtomId::fromProtocol(value).value(); }

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

TEST(DockPropertiesTest, BuildsOwnedExactEwmhPayload) {
    BottomPanelStrut reservation{
        BottomPanelGeometry{1280U, 718U, 1024U, 50U},
        {0U, 0U, 0U, 306U},
        {0U, 0U, 0U, 306U, 0U, 0U, 0U, 0U, 0U, 0U, 1280U, 2303U},
    };

    const auto payload = detail::makeDockPropertyPayload(atom(77U), reservation);
    ASSERT_TRUE(payload);
    reservation.strut.fill(9U);
    reservation.strutPartial.fill(9U);

    EXPECT_EQ(payload.value().windowType, (std::array<std::uint32_t, 1U>{77U}));
    EXPECT_EQ(payload.value().strut, (std::array<std::uint32_t, 4U>{0U, 0U, 0U, 306U}));
    EXPECT_EQ(
        payload.value().strutPartial,
        (std::array<std::uint32_t, 12U>{0U, 0U, 0U, 306U, 0U, 0U, 0U, 0U, 0U, 0U, 1280U, 2303U}));
}

TEST(DockPropertiesTest, RejectsForgedOrInconsistentReservationsWithoutReflectingValues) {
    BottomPanelStrut reservation{
        BottomPanelGeometry{10U, 20U, 100U, 30U},
        {0U, 0U, 0U, 30U},
        {0U, 0U, 0U, 30U, 0U, 0U, 0U, 0U, 0U, 0U, 10U, 109U},
    };
    EXPECT_TRUE(detail::makeDockPropertyPayload(atom(1U), reservation));

    reservation.strutPartial[11] = 0xdeadbeefU;
    const auto rejected = detail::makeDockPropertyPayload(atom(1U), reservation);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(rejected.error().message.find("deadbeef"), std::string::npos);
    EXPECT_EQ(rejected.error().recovery.find("deadbeef"), std::string::npos);

    reservation = BottomPanelStrut{
        BottomPanelGeometry{std::numeric_limits<std::uint32_t>::max(), 0U, 1U, 1U},
        {0U, 0U, 0U, 1U},
        {0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, std::numeric_limits<std::uint32_t>::max(),
         std::numeric_limits<std::uint32_t>::max()},
    };
    EXPECT_FALSE(detail::makeDockPropertyPayload(atom(1U), reservation));
}

TEST(DockPropertiesTest, RejectsRootTargetWithBoundedRedactedDiagnostic) {
    const auto root = window(0xdeadbeefU);
    const auto rejected = detail::validateDockTarget(root, root);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::invalid_argument);
    EXPECT_EQ(rejected.error().message.find("deadbeef"), std::string::npos);
    EXPECT_EQ(rejected.error().recovery.find("deadbeef"), std::string::npos);
    EXPECT_TRUE(detail::validateDockTarget(window(7U), root));
}

} // namespace
} // namespace prismdrake::x11
