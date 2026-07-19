#include "ShellRuntimeComposition.hpp"

#include "OutputSurfaceRegistry.hpp"
#include "OutputTopology.hpp"
#include "ScreenReconciliation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

using composition::AuthoritySnapshot;
using composition::CompositionIntent;
using composition::OwnerEpoch;
using composition::OwnerGeneration;
using composition::SurfaceFamily;
using control::ShellRequestFailure;
using outputs::OutputSurfaceRegistry;
using outputs::PanelOutputPolicy;
using outputs::PanelPlacementPolicy;
using outputs::QtScreenObservation;
using outputs::SurfaceRegistrySnapshot;

[[nodiscard]] x11::OutputId outputId(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] x11::CrtcId crtcId(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] AuthoritySnapshot authority(std::uint64_t epoch, std::uint64_t generation) {
    return {OwnerEpoch::create(epoch).value(), OwnerGeneration::create(generation).value()};
}

class TopologyHarness final {
  public:
    TopologyHarness() : registry_(OutputSurfaceRegistry::create(48U).value()) {}

    [[nodiscard]] SurfaceRegistrySnapshot observe(std::uint64_t generation,
                                                  bool includeRight = true) {
        std::vector<x11::OutputCandidate> candidates{
            {outputId(1U), crtcId(10U), 0, 0, 800U, 600U, "LEFT-1"}};
        std::vector<QtScreenObservation> screens{{"LEFT-1", {0, 0, 800U, 600U}, 1.0}};
        if (includeRight) {
            candidates.push_back({outputId(2U), crtcId(20U), 800, 0, 800U, 600U, "RIGHT-1"});
            screens.push_back({"RIGHT-1", {800, 0, 800U, 600U}, 1.0});
        }
        const auto root = x11::RootGeometry::create(includeRight ? 1600U : 800U, 600U).value();
        auto topology = x11::buildOutputTopology({true, root, candidates.size(), candidates.size(),
                                                  1U, std::move(candidates), outputId(1U)})
                            .value();
        auto reconciled = outputs::reconcileScreens(generation, topology, screens).value();
        return registry_.observe(reconciled, PanelPlacementPolicy{PanelOutputPolicy::all, {}})
            .value();
    }

  private:
    OutputSurfaceRegistry registry_;
};

class PresentationPort final : public ShellRuntimePresentationPort {
  public:
    [[nodiscard]] ShellRequestFailure apply(std::span<const CompositionIntent> intents) override {
        batches.emplace_back(intents.begin(), intents.end());
        return failure;
    }

    [[nodiscard]] ShellRequestFailure focusPanel(const composition::OutputName &output) override {
        focusedPanel = std::string(output.value());
        return failure;
    }

    ShellRequestFailure failure{ShellRequestFailure::none};
    std::vector<std::vector<CompositionIntent>> batches;
    std::optional<std::string> focusedPanel;
};

TEST(ShellRuntimeCompositionTest, RoutesShell1AndQuickSettingsThroughOneOutputGeneration) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_TRUE(runtime.observeNotificationOwner(authority(20U, 1U)));

    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, {}),
              ShellRequestFailure::none);
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().family, SurfaceFamily::launcher);
    EXPECT_EQ(runtime.state().activeSurfaces().front().output.value(), "LEFT-1");

    EXPECT_EQ(runtime.request(control::ShellAction::open_quick_settings, "RIGHT-1"),
              ShellRequestFailure::none);
    const auto quick =
        std::ranges::find(runtime.state().activeSurfaces(), SurfaceFamily::quickSettings,
                          &composition::ActiveSurface::family);
    ASSERT_NE(quick, runtime.state().activeSurfaces().end());
    EXPECT_EQ(quick->output.value(), "RIGHT-1");

    EXPECT_TRUE(runtime.requestOpen().accepted());
    const auto center =
        std::ranges::find(runtime.state().activeSurfaces(), SurfaceFamily::notificationCenter,
                          &composition::ActiveSurface::family);
    ASSERT_NE(center, runtime.state().activeSurfaces().end());
    EXPECT_EQ(center->output.value(), "LEFT-1");

    EXPECT_EQ(runtime.request(control::ShellAction::focus_panel, "RIGHT-1"),
              ShellRequestFailure::none);
    EXPECT_EQ(port.focusedPanel, "RIGHT-1");
    EXPECT_EQ(runtime.request(control::ShellAction::focus_panel, "MISSING-1"),
              ShellRequestFailure::output_unavailable);
}

TEST(ShellRuntimeCompositionTest, OwnerLossAndOutputRemovalProduceAtomicDismissals) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_TRUE(runtime.observeNotificationOwner(authority(20U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::open_notification_center, "RIGHT-1"),
              ShellRequestFailure::none);

    ASSERT_TRUE(runtime.observeNotificationOwner(std::nullopt));
    EXPECT_TRUE(runtime.state().activeSurfaces().empty());
    EXPECT_FALSE(runtime.requestOpen().accepted());

    ASSERT_TRUE(runtime.observeNotificationOwner(authority(21U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::open_quick_settings, "RIGHT-1"),
              ShellRequestFailure::none);
    ASSERT_TRUE(runtime.observeOutputs(topology.observe(2U, false)));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().output.value(), "LEFT-1");

    ASSERT_TRUE(runtime.observeSettingsOwner(std::nullopt));
    EXPECT_TRUE(runtime.state().activeSurfaces().empty());
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, {}),
              ShellRequestFailure::internal);
}

TEST(ShellRuntimeCompositionTest, RejectsStaleCompleteGenerationsAndPresentationFailures) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;
    TopologyHarness staleTopology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(2U)));
    EXPECT_FALSE(runtime.observeOutputs(staleTopology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 2U)));

    port.failure = ShellRequestFailure::timed_out;
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, {}),
              ShellRequestFailure::timed_out);
}

} // namespace
} // namespace prismdrake::shell::runtime
