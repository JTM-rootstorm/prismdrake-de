#include "OutputTopology.hpp"
#include "RandrTopology.hpp"
#include "RootEventStream.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string_view>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] foundation::Result<X11Connection> connectDisplay() {
    const char *display = std::getenv("DISPLAY");
    if (display == nullptr || std::string_view{display}.empty()) {
        return foundation::Result<X11Connection>::failure(
            {foundation::ErrorCode::invalid_environment, "The X11 test display is unavailable.",
             "Run the integration test through the isolated Xvfb harness."});
    }
    return X11Connection::connect(display);
}

TEST(RandrTopologyIntegrationTest, QueriesBoundedActiveOutputsAndSelectsRefreshEvents) {
    auto connection = connectDisplay();
    ASSERT_TRUE(connection);
    auto protocol = RandrTopologyProtocol::negotiate(connection.value());
    ASSERT_TRUE(protocol);
    if (protocol.value().status() == RandrTopologyStatus::unavailable) {
        GTEST_SKIP() << "isolated X server has no RandR 1.2 support";
    }
    ASSERT_NE(protocol.value().status(), RandrTopologyStatus::malformed);

    auto stream = RootEventStream::create(connection.value(), protocol.value());
    ASSERT_TRUE(stream);

    auto snapshot = protocol.value().query(connection.value());
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(snapshot.value().randrAvailable());
    EXPECT_LE(snapshot.value().resourceOutputCount, maximumTopologyOutputs);
    EXPECT_LE(snapshot.value().resourceCrtcCount, maximumTopologyCrtcs);
    EXPECT_LE(snapshot.value().resourceModeCount, maximumTopologyModes);
    EXPECT_LE(snapshot.value().activeOutputs.size(), maximumTopologyOutputs);
    EXPECT_EQ(snapshot.value().coreScreen.rootWindow, connection.value().screen().rootWindow);
    EXPECT_GT(snapshot.value().coreScreen.widthPx, 0U);
    EXPECT_GT(snapshot.value().coreScreen.heightPx, 0U);

    auto root = RootGeometry::fromScreenInfo(snapshot.value().coreScreen);
    ASSERT_TRUE(root);
    auto selected = buildOutputTopology(OutputTopologyObservation{
        snapshot.value().randrAvailable(), root.value(), snapshot.value().resourceOutputCount,
        snapshot.value().resourceCrtcCount, snapshot.value().resourceModeCount,
        snapshot.value().activeOutputs, snapshot.value().primary});
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected.value().randrAvailable(), snapshot.value().randrAvailable());
}

TEST(RandrTopologyIntegrationTest, RejectsUseAcrossConnectionProvenance) {
    auto first = connectDisplay();
    auto second = connectDisplay();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto protocol = RandrTopologyProtocol::negotiate(first.value());
    ASSERT_TRUE(protocol);

    const auto query = protocol.value().query(second.value());
    const auto subscription = RootEventStream::create(second.value(), protocol.value());

    ASSERT_FALSE(query);
    EXPECT_EQ(query.error().code, foundation::ErrorCode::invalid_argument);
    ASSERT_FALSE(subscription);
    EXPECT_EQ(subscription.error().code, foundation::ErrorCode::invalid_argument);

    // Provenance validation happens before acquiring the sole stream or
    // selecting any events on the second connection.
    EXPECT_TRUE(RootEventStream::create(second.value()));
}

TEST(RandrTopologyIntegrationTest, UsesFreshCoreRootFallbackWithoutRandr) {
    auto connection = connectDisplay();
    ASSERT_TRUE(connection);
    auto protocol = RandrTopologyProtocol::negotiate(connection.value());
    ASSERT_TRUE(protocol);
    if (protocol.value().status() != RandrTopologyStatus::unavailable) {
        GTEST_SKIP() << "run this case with Xvfb -extension RANDR";
    }

    auto snapshot = protocol.value().query(connection.value());

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().status, RandrTopologyStatus::unavailable);
    EXPECT_FALSE(snapshot.value().randrAvailable());
    EXPECT_TRUE(snapshot.value().activeOutputs.empty());
    EXPECT_FALSE(snapshot.value().primary);
    EXPECT_EQ(snapshot.value().resourceOutputCount, 0U);
    EXPECT_EQ(snapshot.value().resourceCrtcCount, 0U);
    EXPECT_EQ(snapshot.value().resourceModeCount, 0U);
    EXPECT_EQ(snapshot.value().coreScreen.rootWindow, connection.value().screen().rootWindow);
    EXPECT_GT(snapshot.value().coreScreen.widthPx, 0U);
    EXPECT_GT(snapshot.value().coreScreen.heightPx, 0U);
}

} // namespace
} // namespace prismdrake::x11
