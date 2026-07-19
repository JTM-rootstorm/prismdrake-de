#include "OutputSurfaceRegistry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::shell::outputs {
namespace {

[[nodiscard]] x11::OutputId outputId(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] x11::CrtcId crtcId(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] x11::OutputCandidate candidate(std::uint32_t output, std::uint32_t crtc,
                                             std::string name, std::int64_t x, std::int64_t y,
                                             std::uint64_t width, std::uint64_t height) {
    return {outputId(output), crtcId(crtc), x, y, width, height, std::move(name)};
}

[[nodiscard]] x11::OutputTopologySnapshot nativeTopology(std::vector<x11::OutputCandidate> outputs,
                                                         std::uint32_t primary,
                                                         std::uint64_t rootWidth = 4480U,
                                                         std::uint64_t rootHeight = 1440U) {
    const auto root = x11::RootGeometry::create(rootWidth, rootHeight).value();
    return x11::buildOutputTopology({true, root, outputs.size(), outputs.size(), 2U,
                                     std::move(outputs), outputId(primary)})
        .value();
}

[[nodiscard]] ReconciledTopologySnapshot twoOutputs(std::uint64_t generation,
                                                    std::uint32_t primary = 1U) {
    const auto topology = nativeTopology({candidate(1U, 10U, "HDMI-1", 2560, 0, 1920U, 1080U),
                                          candidate(2U, 20U, "DP-1", 0, 0, 2560U, 1440U)},
                                         primary);
    const std::array screens{
        QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
        QtScreenObservation{"DP-1", {-1280, 0, 1280U, 720U}, 2.0},
    };
    return reconcileScreens(generation, topology, screens).value();
}

[[nodiscard]] bool hasEvent(std::span<const TopologyEvent> events, TopologyEventKind kind,
                            std::string_view output) {
    return std::any_of(events.begin(), events.end(), [kind, output](const auto &event) {
        return event.kind == kind && event.outputName == output;
    });
}

TEST(ScreenReconciliationTest, PreservesStableNamesMixedScaleAndNegativeLogicalCoordinates) {
    const auto snapshot = twoOutputs(7U);

    ASSERT_EQ(snapshot.generation(), 7U);
    ASSERT_EQ(snapshot.outputs().size(), 2U);
    EXPECT_EQ(snapshot.primaryOutputName(), "HDMI-1");
    const auto dp = std::find_if(snapshot.outputs().begin(), snapshot.outputs().end(),
                                 [](const auto &output) { return output.name == "DP-1"; });
    ASSERT_NE(dp, snapshot.outputs().end());
    ASSERT_TRUE(dp->available());
    EXPECT_EQ(dp->logicalGeometry, (LogicalRect{-1280, 0, 1280U, 720U}));
    EXPECT_DOUBLE_EQ(dp->devicePixelRatio, 2.0);
    EXPECT_EQ(dp->nativeGeometry.xPx(), 0U);
}

TEST(ScreenReconciliationTest, KeepsMissingAndMismatchedScreensExplicitlyUnavailable) {
    const auto topology = nativeTopology({candidate(1U, 10U, "HDMI-1", 0, 0, 1920U, 1080U),
                                          candidate(2U, 20U, "DP-1", 1920, 0, 2560U, 1440U)},
                                         2U);
    const std::array screens{
        QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
        QtScreenObservation{"DP-1", {1920, 0, 1279U, 720U}, 2.0},
    };

    const auto reconciled = reconcileScreens(1U, topology, screens);

    ASSERT_TRUE(reconciled);
    EXPECT_EQ(reconciled.value().primaryOutputName(), "HDMI-1");
    EXPECT_EQ(reconciled.value().outputs()[1].status, ReconciliationStatus::geometryMismatch);
    EXPECT_FALSE(reconciled.value().outputs()[1].available());
}

TEST(ScreenReconciliationTest, RejectsContradictoryRelativeScreenGeometryFromPlacement) {
    const auto topology = nativeTopology({candidate(1U, 10U, "left", 0, 0, 1000U, 1000U),
                                          candidate(2U, 20U, "right", 1000, 0, 1000U, 1000U)},
                                         1U, 2000U, 1000U);
    const std::array screens{
        QtScreenObservation{"left", {1000, 0, 1000U, 1000U}, 1.0},
        QtScreenObservation{"right", {0, 0, 1000U, 1000U}, 1.0},
    };

    const auto reconciled = reconcileScreens(1U, topology, screens);

    ASSERT_TRUE(reconciled);
    EXPECT_FALSE(reconciled.value().primaryOutputName());
    EXPECT_TRUE(std::all_of(reconciled.value().outputs().begin(),
                            reconciled.value().outputs().end(), [](const auto &output) {
                                return output.status == ReconciliationStatus::geometryMismatch &&
                                       !output.available();
                            }));
}

TEST(ScreenReconciliationTest, RejectsUnsafeDuplicateAndUnboundedValuesAtomically) {
    EXPECT_TRUE(x11::isStableOutputName("DP-1"));
    EXPECT_TRUE(x11::isStableOutputName("Écran-1"));
    EXPECT_TRUE(x11::isStableOutputName("display one"));
    EXPECT_FALSE(x11::isStableOutputName(""));
    EXPECT_FALSE(x11::isStableOutputName("bad\nname"));
    EXPECT_FALSE(x11::isStableOutputName(std::string{"\xc0\xaf", 2U}));
    EXPECT_FALSE(x11::isStableOutputName(std::string(x11::maximumOutputNameBytes + 1U, 'x')));

    const auto duplicateRoot = x11::RootGeometry::create(2000U, 1000U).value();
    EXPECT_FALSE(x11::buildOutputTopology({true,
                                           duplicateRoot,
                                           2U,
                                           2U,
                                           1U,
                                           {candidate(1U, 10U, "duplicate", 0, 0, 1000U, 1000U),
                                            candidate(2U, 20U, "duplicate", 1000, 0, 1000U, 1000U)},
                                           outputId(1U)}));

    const auto topology =
        nativeTopology({candidate(1U, 10U, "HDMI-1", 0, 0, 1920U, 1080U)}, 1U, 1920U, 1080U);
    const std::array duplicate{
        QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
        QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
    };
    EXPECT_FALSE(reconcileScreens(1U, topology, duplicate));
    EXPECT_FALSE(reconcileScreens(0U, topology, std::span<const QtScreenObservation>{}));

    const std::array invalidScale{
        QtScreenObservation{
            "HDMI-1", {0, 0, 1920U, 1080U}, std::numeric_limits<double>::infinity()},
    };
    EXPECT_FALSE(reconcileScreens(1U, topology, invalidScale));
}

TEST(OutputSurfaceRegistryTest, CreatesDesktopAndIndependentPanelPerAvailableOutput) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    const auto update = registry.observe(twoOutputs(1U), {PanelOutputPolicy::all, {}});

    ASSERT_TRUE(update);
    ASSERT_EQ(update.value().desktops().size(), 2U);
    ASSERT_EQ(update.value().panels().size(), 2U);
    EXPECT_TRUE(hasEvent(update.value().events(), TopologyEventKind::added, "HDMI-1"));
    EXPECT_TRUE(hasEvent(update.value().events(), TopologyEventKind::added, "DP-1"));

    const auto hdmi = std::find_if(update.value().panels().begin(), update.value().panels().end(),
                                   [](const auto &panel) { return panel.outputName == "HDMI-1"; });
    const auto dp = std::find_if(update.value().panels().begin(), update.value().panels().end(),
                                 [](const auto &panel) { return panel.outputName == "DP-1"; });
    ASSERT_NE(hdmi, update.value().panels().end());
    ASSERT_NE(dp, update.value().panels().end());
    ASSERT_TRUE(hdmi->panelStrut);
    ASSERT_TRUE(dp->panelStrut);
    EXPECT_EQ(hdmi->panelStrut->strutPartial[10], 2560U);
    EXPECT_EQ(hdmi->panelStrut->strutPartial[11], 4479U);
    EXPECT_EQ(dp->panelStrut->strutPartial[10], 0U);
    EXPECT_EQ(dp->panelStrut->strutPartial[11], 2559U);
    EXPECT_EQ(dp->panelStrut->panel.height, 96U);
    EXPECT_EQ(dp->logicalGeometry, (LogicalRect{-1280, 672, 1280U, 48U}));
}

TEST(OutputSurfaceRegistryTest, PrimaryAndNamedPoliciesReevaluateWithoutRestart) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    auto first = registry.observe(twoOutputs(10U, 1U), {PanelOutputPolicy::primary, {}});
    ASSERT_TRUE(first);
    ASSERT_EQ(first.value().panels().size(), 1U);
    EXPECT_EQ(first.value().panels()[0].outputName, "HDMI-1");

    auto second = registry.observe(twoOutputs(11U, 2U), {PanelOutputPolicy::primary, {}});
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().panels()[0].outputName, "DP-1");
    EXPECT_TRUE(hasEvent(second.value().events(), TopologyEventKind::primaryChanged, "DP-1"));

    auto third =
        registry.observe(twoOutputs(12U, 1U), {PanelOutputPolicy::named, {"disconnected", "DP-1"}});
    ASSERT_TRUE(third);
    ASSERT_EQ(third.value().panels().size(), 1U);
    EXPECT_EQ(third.value().panels()[0].outputName, "DP-1");

    auto fallback =
        registry.observe(twoOutputs(13U, 1U), {PanelOutputPolicy::named, {"disconnected"}});
    ASSERT_TRUE(fallback);
    ASSERT_EQ(fallback.value().panels().size(), 1U);
    EXPECT_EQ(fallback.value().panels()[0].outputName, "HDMI-1");
}

TEST(OutputSurfaceRegistryTest, ReconcilesNewSettingsPolicyWithoutForgingTopologyGeneration) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    const auto topology = twoOutputs(10U, 1U);
    auto primary = registry.observe(topology, {PanelOutputPolicy::primary, {}}, 20U);
    ASSERT_TRUE(primary);
    ASSERT_EQ(primary.value().panels().size(), 1U);

    auto all = registry.observe(topology, {PanelOutputPolicy::all, {}}, 21U);
    ASSERT_TRUE(all);
    EXPECT_EQ(all.value().topologyGeneration(), 10U);
    EXPECT_EQ(all.value().settingsGeneration(), 21U);
    EXPECT_GT(all.value().surfaceGeneration(), primary.value().surfaceGeneration());
    ASSERT_EQ(all.value().panels().size(), 2U);

    auto named = registry.observe(topology, {PanelOutputPolicy::named, {"DP-1"}}, 22U);
    ASSERT_TRUE(named);
    ASSERT_EQ(named.value().panels().size(), 1U);
    EXPECT_EQ(named.value().panels()[0].outputName, "DP-1");

    const auto repeated = registry.observe(topology, {PanelOutputPolicy::named, {"DP-1"}}, 22U);
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.value().surfaceGeneration(), named.value().surfaceGeneration());
    EXPECT_FALSE(registry.observe(topology, {PanelOutputPolicy::all, {}}, 22U));
    EXPECT_FALSE(registry.observe(topology, {PanelOutputPolicy::primary, {}}, 21U));
    ASSERT_TRUE(registry.current());
    EXPECT_EQ(registry.current()->panels()[0].outputName, "DP-1");
}

TEST(OutputSurfaceRegistryTest, PublishesGeometryAndScaleChangesAsOneNewGeneration) {
    const auto native =
        nativeTopology({candidate(1U, 10U, "HDMI-1", 0, 0, 1920U, 1080U)}, 1U, 1920U, 1080U);
    const std::array firstScreen{
        QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
    };
    const std::array scaledScreen{
        QtScreenObservation{"HDMI-1", {-960, 0, 960U, 540U}, 2.0},
    };
    auto registry = OutputSurfaceRegistry::create(48U).value();
    ASSERT_TRUE(registry.observe(reconcileScreens(1U, native, firstScreen).value(),
                                 {PanelOutputPolicy::primary, {}}));

    auto update = registry.observe(reconcileScreens(2U, native, scaledScreen).value(),
                                   {PanelOutputPolicy::primary, {}});

    ASSERT_TRUE(update);
    EXPECT_EQ(update.value().topologyGeneration(), 2U);
    EXPECT_TRUE(hasEvent(update.value().events(), TopologyEventKind::geometryChanged, "HDMI-1"));
    EXPECT_TRUE(hasEvent(update.value().events(), TopologyEventKind::scaleChanged, "HDMI-1"));
    ASSERT_EQ(update.value().desktops().size(), 1U);
    EXPECT_EQ(update.value().desktops()[0].topologyGeneration, 2U);
    EXPECT_EQ(update.value().desktops()[0].logicalGeometry, (LogicalRect{-960, 0, 960U, 540U}));
}

TEST(OutputSurfaceRegistryTest, RemovalMigratesVisibleTransientsOrDismissesWithoutOutputs) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    ASSERT_TRUE(registry.observe(twoOutputs(1U), {PanelOutputPolicy::all, {}}));
    auto launcher = registry.anchorTransient(SurfaceKind::launcher, "DP-1");
    auto notifications = registry.anchorTransient(SurfaceKind::transientNotifications, "DP-1");
    ASSERT_TRUE(launcher);
    ASSERT_TRUE(notifications);
    EXPECT_EQ(launcher.value().outputName, "DP-1");
    EXPECT_EQ(notifications.value().outputName, "HDMI-1");

    const auto native =
        nativeTopology({candidate(3U, 30U, "eDP-1", 0, 0, 1920U, 1080U)}, 3U, 1920U, 1080U);
    const std::array screen{QtScreenObservation{"eDP-1", {0, 0, 1920U, 1080U}, 1.0}};
    const auto single = reconcileScreens(2U, native, screen).value();
    auto migrated = registry.observe(single, {PanelOutputPolicy::all, {}});
    ASSERT_TRUE(migrated);
    ASSERT_EQ(migrated.value().transients().size(), 2U);
    EXPECT_TRUE(std::all_of(migrated.value().transients().begin(),
                            migrated.value().transients().end(),
                            [](const auto &placement) { return placement.outputName == "eDP-1"; }));
    EXPECT_TRUE(hasEvent(migrated.value().events(), TopologyEventKind::removed, "DP-1"));
    EXPECT_TRUE(hasEvent(migrated.value().events(), TopologyEventKind::added, "eDP-1"));

    const auto emptyNative = nativeTopology({}, 99U, 1920U, 1080U);
    const auto empty =
        reconcileScreens(3U, emptyNative, std::span<const QtScreenObservation>{}).value();
    auto dismissed = registry.observe(empty, {PanelOutputPolicy::all, {}});
    ASSERT_TRUE(dismissed);
    EXPECT_TRUE(dismissed.value().desktops().empty());
    EXPECT_TRUE(dismissed.value().panels().empty());
    EXPECT_TRUE(dismissed.value().transients().empty());
}

TEST(OutputSurfaceRegistryTest, RejectsStaleGenerationAndMalformedNamedPolicy) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    ASSERT_TRUE(registry.observe(twoOutputs(4U), {PanelOutputPolicy::primary, {}}));
    EXPECT_TRUE(registry.observe(twoOutputs(4U), {PanelOutputPolicy::primary, {}}));
    auto ignoredNames =
        registry.observe(twoOutputs(5U), {PanelOutputPolicy::all, {"untrusted unused name\n"}});
    ASSERT_TRUE(ignoredNames);
    EXPECT_EQ(ignoredNames.value().panels().size(), 2U);
    EXPECT_FALSE(registry.observe(twoOutputs(6U), {PanelOutputPolicy::named, {"DP-1", "DP-1"}}));
}

TEST(OutputSurfaceRegistryTest, RejectsUnknownExplicitInvocationButMigratesExistingIntent) {
    auto registry = OutputSurfaceRegistry::create(48U).value();
    ASSERT_TRUE(registry.observe(twoOutputs(1U), {PanelOutputPolicy::primary, {}}));

    EXPECT_FALSE(registry.anchorTransient(SurfaceKind::launcher, "missing-output"));
    auto launcher = registry.anchorTransient(SurfaceKind::launcher, "DP-1");
    ASSERT_TRUE(launcher);
    EXPECT_EQ(launcher.value().outputName, "DP-1");

    const auto native =
        nativeTopology({candidate(3U, 30U, "eDP-1", 0, 0, 1920U, 1080U)}, 3U, 1920U, 1080U);
    const std::array screen{QtScreenObservation{"eDP-1", {0, 0, 1920U, 1080U}, 1.0}};
    auto migrated = registry.observe(reconcileScreens(2U, native, screen).value(),
                                     {PanelOutputPolicy::primary, {}});
    ASSERT_TRUE(migrated);
    ASSERT_EQ(migrated.value().transients().size(), 1U);
    EXPECT_EQ(migrated.value().transients()[0].outputName, "eDP-1");
}

TEST(PanelStrutSetTest, DerivesOneUnequalReservationIntervalPerOutput) {
    const auto root = x11::RootGeometry::create(3200U, 1080U).value();
    const auto left = x11::OutputGeometry::create(root, 0, 0, 1920U, 1080U).value();
    const auto right = x11::OutputGeometry::create(root, 1920, 0, 1280U, 720U).value();
    const std::array requests{x11::BottomPanelStrutRequest{left, 48U},
                              x11::BottomPanelStrutRequest{right, 64U}};

    const auto struts = x11::calculateBottomPanelStruts(root, requests);

    ASSERT_TRUE(struts);
    ASSERT_EQ(struts.value().size(), 2U);
    EXPECT_EQ(struts.value()[0].strutPartial[10], 0U);
    EXPECT_EQ(struts.value()[0].strutPartial[11], 1919U);
    EXPECT_EQ(struts.value()[1].strutPartial[10], 1920U);
    EXPECT_EQ(struts.value()[1].strutPartial[11], 3199U);
    EXPECT_EQ(struts.value()[1].strut[3], 424U);
}

} // namespace
} // namespace prismdrake::shell::outputs
