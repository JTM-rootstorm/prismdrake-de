#include "PanelWindowController.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace prismdrake::shell::window {
namespace {

[[nodiscard]] x11::WindowId window(std::uint32_t value) {
    return x11::WindowId::fromProtocol(value).value();
}

[[nodiscard]] x11::OutputId output(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] x11::CrtcId crtc(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] x11::OutputCandidate candidate(std::uint32_t outputId, std::uint32_t crtcId,
                                             std::int64_t x, std::int64_t y, std::uint64_t width,
                                             std::uint64_t height) {
    return {output(outputId), crtc(crtcId), x, y, width, height};
}

[[nodiscard]] x11::RandrTopologySnapshot
snapshot(std::uint32_t width, std::uint32_t height, std::vector<x11::OutputCandidate> activeOutputs,
         std::optional<x11::OutputId> primary = std::nullopt) {
    return {x11::RandrTopologyStatus::randr_1_4,
            {0U, window(1U), width, height},
            activeOutputs.size(),
            activeOutputs.size(),
            1U,
            std::move(activeOutputs),
            primary};
}

TEST(PanelWindowControllerTest, RejectsInvalidPanelHeights) {
    EXPECT_FALSE(PanelWindowController::create(0U));
    EXPECT_FALSE(PanelWindowController::create(std::numeric_limits<std::uint32_t>::max()));
    EXPECT_TRUE(PanelWindowController::create(48U));
}

TEST(PanelWindowControllerTest, PlacesOneBottomPanelOnTheDocumentedPrimaryOutput) {
    auto controller = PanelWindowController::create(48U).value();
    const auto observed = controller.observe(snapshot(
        3200U, 1080U,
        {candidate(10U, 20U, 0, 0, 1280U, 1024U), candidate(11U, 21U, 1280, 0, 1920U, 1080U)},
        output(11U)));

    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value().selectionReason, x11::OutputSelectionReason::randrPrimary);
    EXPECT_EQ(observed.value().dock.panel, (x11::BottomPanelGeometry{1280U, 1032U, 1920U, 48U}));
    EXPECT_EQ(observed.value().dock.strutPartial[10U], 1280U);
    EXPECT_EQ(observed.value().dock.strutPartial[11U], 3199U);
    EXPECT_EQ(controller.current(), observed.value());
}

TEST(PanelWindowControllerTest, RecomputesPlacementWhenThePrimaryOutputChanges) {
    auto controller = PanelWindowController::create(32U).value();
    const std::vector<x11::OutputCandidate> outputs{candidate(10U, 20U, 0, 0, 1920U, 1080U),
                                                    candidate(11U, 21U, 1920, 0, 1280U, 1024U)};

    ASSERT_TRUE(controller.observe(snapshot(3200U, 1080U, outputs, output(10U))));
    ASSERT_EQ(controller.current()->dock.panel, (x11::BottomPanelGeometry{0U, 1048U, 1920U, 32U}));

    ASSERT_TRUE(controller.observe(snapshot(3200U, 1080U, outputs, output(11U))));
    EXPECT_EQ(controller.current()->dock.panel,
              (x11::BottomPanelGeometry{1920U, 992U, 1280U, 32U}));
    EXPECT_EQ(controller.current()->selectionReason, x11::OutputSelectionReason::randrPrimary);
}

TEST(PanelWindowControllerTest, UsesFreshCoreRootWhenRandrIsUnavailable) {
    auto controller = PanelWindowController::create(40U).value();
    const x11::RandrTopologySnapshot unavailable{x11::RandrTopologyStatus::unavailable,
                                                 {0U, window(1U), 1366U, 768U},
                                                 0U,
                                                 0U,
                                                 0U,
                                                 {},
                                                 std::nullopt};

    const auto observed = controller.observe(unavailable);

    ASSERT_TRUE(observed);
    EXPECT_EQ(observed.value().selectionReason, x11::OutputSelectionReason::coreRootFallback);
    EXPECT_EQ(observed.value().dock.panel, (x11::BottomPanelGeometry{0U, 728U, 1366U, 40U}));
}

TEST(PanelWindowControllerTest, RetainsLastValidPlacementAfterMalformedRefresh) {
    auto controller = PanelWindowController::create(48U).value();
    ASSERT_TRUE(controller.observe(
        snapshot(1920U, 1080U, {candidate(10U, 20U, 0, 0, 1920U, 1080U)}, output(10U))));
    const auto previous = controller.current();

    auto malformed =
        snapshot(1920U, 1080U,
                 {candidate(10U, 20U, 0, 0, 1920U, 1080U), candidate(10U, 21U, 0, 0, 1920U, 1080U)},
                 output(10U));
    malformed.resourceOutputCount = 2U;
    malformed.resourceCrtcCount = 2U;

    EXPECT_FALSE(controller.observe(malformed));
    EXPECT_EQ(controller.current(), previous);
}

TEST(PanelWindowControllerTest, RetainsPlacementWhenThePanelNoLongerFitsAnOutput) {
    auto controller = PanelWindowController::create(64U).value();
    ASSERT_TRUE(controller.observe(
        snapshot(800U, 600U, {candidate(1U, 1U, 0, 0, 800U, 600U)}, output(1U))));
    const auto previous = controller.current();

    EXPECT_FALSE(
        controller.observe(snapshot(800U, 600U, {candidate(1U, 1U, 0, 0, 800U, 32U)}, output(1U))));
    EXPECT_EQ(controller.current(), previous);
}

TEST(PanelWindowControllerTest, StagesRuntimeHeightWithoutPublishingPartialState) {
    auto controller = PanelWindowController::create(48U).value();
    const auto topology =
        snapshot(1920U, 1080U, {candidate(1U, 1U, 0, 0, 1920U, 1080U)}, output(1U));
    ASSERT_TRUE(controller.observe(topology));
    const auto previous = controller.current();

    auto update = controller.prepare(topology, 72U);

    ASSERT_TRUE(update);
    EXPECT_EQ(update.value().panelHeight(), 72U);
    EXPECT_EQ(update.value().placement().dock.panel.height, 72U);
    EXPECT_EQ(controller.panelHeight(), 48U);
    EXPECT_EQ(controller.current(), previous);

    controller.commit(std::move(update).value());
    EXPECT_EQ(controller.panelHeight(), 72U);
    EXPECT_EQ(controller.current()->dock.panel.height, 72U);
}

TEST(PanelWindowControllerTest, RejectsInvalidRuntimeHeightWithoutChangingAppliedState) {
    auto controller = PanelWindowController::create(48U).value();
    const auto topology = snapshot(800U, 600U, {candidate(1U, 1U, 0, 0, 800U, 600U)}, output(1U));
    ASSERT_TRUE(controller.observe(topology));
    const auto previous = controller.current();

    EXPECT_FALSE(controller.prepare(topology, 0U));
    EXPECT_FALSE(controller.prepare(topology, 601U));
    EXPECT_EQ(controller.panelHeight(), 48U);
    EXPECT_EQ(controller.current(), previous);
}

} // namespace
} // namespace prismdrake::shell::window
