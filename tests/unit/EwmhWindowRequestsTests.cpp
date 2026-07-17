#include "EwmhWindowRequestsInternal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

[[nodiscard]] AtomId atom(std::uint32_t value) { return AtomId::fromProtocol(value).value(); }

TEST(EwmhWindowRequestsTest, EncodesPagerActivationWithUserTimestampAndCurrentWindow) {
    const auto request =
        detail::encodeActivateRequest(window(10U), atom(20U), 0x12345678U, window(11U));

    EXPECT_EQ(request.responseType, detail::clientMessageResponseType);
    EXPECT_EQ(request.format, 32U);
    EXPECT_EQ(request.sequence, 0U);
    EXPECT_EQ(request.target, window(10U));
    EXPECT_EQ(request.messageType, atom(20U));
    EXPECT_EQ(request.data, (std::array<std::uint32_t, 5>{2U, 0x12345678U, 11U, 0U, 0U}));
    EXPECT_EQ(request.destinationMask,
              detail::substructureNotifyMask | detail::substructureRedirectMask);
}

TEST(EwmhWindowRequestsTest, EncodesMissingCurrentWindowAsNoneAndPreservesCurrentTime) {
    const auto request = detail::encodeActivateRequest(window(10U), atom(20U), 0U, std::nullopt);

    EXPECT_EQ(request.data, (std::array<std::uint32_t, 5>{2U, 0U, 0U, 0U, 0U}));
}

TEST(EwmhWindowRequestsTest, EncodesCloseThroughWindowManagerOnly) {
    const auto request = detail::encodeCloseRequest(window(30U), atom(40U), 99U);

    EXPECT_EQ(request.responseType, detail::clientMessageResponseType);
    EXPECT_EQ(request.format, detail::clientMessageFormat);
    EXPECT_EQ(request.sequence, 0U);
    EXPECT_EQ(request.target, window(30U));
    EXPECT_EQ(request.messageType, atom(40U));
    EXPECT_EQ(request.data, (std::array<std::uint32_t, 5>{99U, 2U, 0U, 0U, 0U}));
    EXPECT_EQ(request.destinationMask, detail::ewmhRequestEventMask);
}

TEST(EwmhWindowRequestsTest, EncodesIcccmIconicStateMinimizeThroughWindowManager) {
    const auto request = detail::encodeMinimizeRequest(window(50U), atom(60U));

    EXPECT_EQ(request.responseType, detail::clientMessageResponseType);
    EXPECT_EQ(request.format, detail::clientMessageFormat);
    EXPECT_EQ(request.sequence, 0U);
    EXPECT_EQ(request.target, window(50U));
    EXPECT_EQ(request.messageType, atom(60U));
    EXPECT_EQ(request.data, (std::array<std::uint32_t, 5>{3U, 0U, 0U, 0U, 0U}));
    EXPECT_EQ(request.destinationMask,
              detail::substructureNotifyMask | detail::substructureRedirectMask);
}

} // namespace
} // namespace prismdrake::x11
