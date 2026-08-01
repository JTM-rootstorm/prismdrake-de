#include "ShellReadinessGate.hpp"

#include <gtest/gtest.h>

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] ShellReadinessObservation completeReadiness() {
    return {.processStarted = true,
            .settingsOwnerAvailable = true,
            .presentationSurfacesReady = true,
            .hasPrimaryOutput = true,
            .settingsGeneration = 7U,
            .outputSettingsGeneration = 7U,
            .outputSurfaceGeneration = 11U,
            .panelSurfaceGeneration = 11U,
            .outputTopologyGeneration = 5U,
            .desktopTopologyGeneration = 5U,
            .wallpaperSettingsGeneration = 7U,
            .outputDesktopCount = 2U,
            .desktopSurfaceCount = 2U};
}

TEST(ShellReadinessGateTest, WaitsForOneCompleteCrossComponentGeneration) {
    ShellReadinessGate gate;
    auto observation = completeReadiness();

    observation.processStarted = false;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.processStarted = true;
    observation.wallpaperSettingsGeneration = 6U;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.wallpaperSettingsGeneration = 7U;
    observation.panelSurfaceGeneration = 10U;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.panelSurfaceGeneration = 11U;
    observation.desktopTopologyGeneration = 4U;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.desktopTopologyGeneration = 5U;
    observation.desktopSurfaceCount = 1U;
    EXPECT_FALSE(gate.consumeIfComplete(observation));

    observation.desktopSurfaceCount = 2U;
    EXPECT_TRUE(gate.consumeIfComplete(observation));
    EXPECT_TRUE(gate.consumed());
    EXPECT_FALSE(gate.consumeIfComplete(observation));
}

TEST(ShellReadinessGateTest, RequiresPrimaryOutputAndPresentationSurfaces) {
    ShellReadinessGate gate;
    auto observation = completeReadiness();

    observation.hasPrimaryOutput = false;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.hasPrimaryOutput = true;
    observation.presentationSurfacesReady = false;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
    observation.presentationSurfacesReady = true;
    observation.settingsOwnerAvailable = false;
    EXPECT_FALSE(gate.consumeIfComplete(observation));
}

} // namespace
} // namespace prismdrake::shell::runtime
