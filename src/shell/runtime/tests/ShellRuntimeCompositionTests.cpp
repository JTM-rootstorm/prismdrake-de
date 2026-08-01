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
        events.emplace_back("apply");
        batches.emplace_back(intents.begin(), intents.end());
        return failure;
    }

    [[nodiscard]] ShellRequestFailure focusPanel(const composition::OutputName &output) override {
        focusedPanel = std::string(output.value());
        return failure;
    }

    [[nodiscard]] ShellRequestFailure releasePanelKeyboardAccess() override {
        events.emplace_back("release_panel_keyboard");
        ++panelKeyboardReleases;
        return releaseFailure;
    }

    ShellRequestFailure failure{ShellRequestFailure::none};
    ShellRequestFailure releaseFailure{ShellRequestFailure::none};
    std::vector<std::vector<CompositionIntent>> batches;
    std::vector<std::string> events;
    std::size_t panelKeyboardReleases{0U};
    std::optional<std::string> focusedPanel;
};

TEST(ShellRuntimeCompositionTest, ReleasesPanelKeyboardBeforeFocusedSingletonPresentation) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    port.events.clear();
    port.panelKeyboardReleases = 0U;

    EXPECT_EQ(runtime.request(control::ShellAction::open_quick_settings, "LEFT-1"),
              ShellRequestFailure::none);
    EXPECT_EQ(port.events, (std::vector<std::string>{"release_panel_keyboard", "apply"}));
    EXPECT_EQ(port.panelKeyboardReleases, 1U);

    PresentationPort rejecting;
    ShellRuntimeComposition rejectedRuntime{rejecting};
    ASSERT_TRUE(rejectedRuntime.observeOutputs(topology.observe(2U)));
    ASSERT_TRUE(rejectedRuntime.observeSettingsOwner(authority(11U, 1U)));
    rejecting.releaseFailure = ShellRequestFailure::internal;
    rejecting.events.clear();
    rejecting.batches.clear();
    EXPECT_EQ(rejectedRuntime.request(control::ShellAction::open_quick_settings, "LEFT-1"),
              ShellRequestFailure::internal);
    EXPECT_EQ(rejecting.events, (std::vector<std::string>{"release_panel_keyboard"}));
    EXPECT_TRUE(rejecting.batches.empty());
}

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
    EXPECT_EQ(runtime.request(control::ShellAction::focus_panel, "bad\noutput"),
              ShellRequestFailure::invalid_output);

    const auto active = runtime.state().activeSurfaces();
    const std::vector<composition::ActiveSurface> activeBeforeDeadline{active.begin(),
                                                                       active.end()};
    const auto batchesBeforeDeadline = port.batches.size();
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, "LEFT-1",
                              foundation::MonotonicClock::TimePoint{}),
              ShellRequestFailure::timed_out);
    EXPECT_TRUE(std::ranges::equal(runtime.state().activeSurfaces(), activeBeforeDeadline));
    EXPECT_EQ(port.batches.size(), batchesBeforeDeadline);

    port.focusedPanel.reset();
    EXPECT_EQ(runtime.request(control::ShellAction::focus_panel, "LEFT-1",
                              foundation::MonotonicClock::TimePoint{}),
              ShellRequestFailure::timed_out);
    EXPECT_FALSE(port.focusedPanel.has_value());
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
              ShellRequestFailure::busy);
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
    EXPECT_TRUE(runtime.state().activeSurfaces().empty());

    port.failure = ShellRequestFailure::none;
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, {}),
              ShellRequestFailure::none);
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);

    port.failure = ShellRequestFailure::internal;
    EXPECT_FALSE(runtime.observeSettingsOwner(std::nullopt));
    EXPECT_EQ(runtime.state().activeSurfaces().size(), 1U);

    port.failure = ShellRequestFailure::none;
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, {}),
              ShellRequestFailure::none);
    EXPECT_TRUE(runtime.state().activeSurfaces().empty());
}

TEST(ShellRuntimeCompositionTest, FailedTopologyApplicationRetainsPreviousCompleteTopology) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::toggle_launcher, "RIGHT-1"),
              ShellRequestFailure::none);

    port.failure = ShellRequestFailure::timed_out;
    EXPECT_FALSE(runtime.observeOutputs(topology.observe(2U, false)));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().output.value(), "RIGHT-1");

    port.failure = ShellRequestFailure::none;
    EXPECT_EQ(runtime.request(control::ShellAction::focus_panel, "RIGHT-1"),
              ShellRequestFailure::none);
    EXPECT_EQ(port.focusedPanel, "RIGHT-1");
}

TEST(ShellRuntimeCompositionTest, LogoutModalPreemptsSurfacesAndEscapeRequestsCancellation) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::open_quick_settings, "RIGHT-1"),
              ShellRequestFailure::none);

    ASSERT_TRUE(runtime.present(SurfaceFamily::logoutConfirmation, "LEFT-1",
                                composition::FocusOrigin::windowManager()));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().family, SurfaceFamily::logoutConfirmation);
    EXPECT_EQ(runtime.request(control::ShellAction::toggle_launcher, "RIGHT-1"),
              ShellRequestFailure::busy);

    ASSERT_TRUE(runtime.escape());
    ASSERT_FALSE(port.batches.empty());
    EXPECT_TRUE(
        std::holds_alternative<composition::CancelLogoutIntent>(port.batches.back().front()));
    EXPECT_EQ(runtime.state().activeSurfaces().front().family, SurfaceFamily::logoutConfirmation);

    ASSERT_TRUE(runtime.dismiss(SurfaceFamily::logoutConfirmation));
    EXPECT_TRUE(runtime.state().activeSurfaces().empty());
}

TEST(ShellRuntimeCompositionTest, LauncherActionMenuRetainsParentAndRestoresExactResultOrigin) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::toggle_launcher, "LEFT-1"),
              ShellRequestFailure::none);
    auto output = composition::OutputName::create("LEFT-1").value();
    auto controlId = composition::FocusControlId::create("launcher-result").value();
    auto origin = composition::FocusOrigin::control(
                      output, composition::FocusControlRole::launcherResult, controlId)
                      .value();

    ASSERT_TRUE(runtime.present(SurfaceFamily::actionMenu, "LEFT-1", origin));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 2U);
    EXPECT_NE(std::ranges::find(runtime.state().activeSurfaces(), SurfaceFamily::launcher,
                                &composition::ActiveSurface::family),
              runtime.state().activeSurfaces().end());
    const auto &openBatch = port.batches.back();
    EXPECT_FALSE(std::ranges::any_of(openBatch, [](const auto &intent) {
        const auto *dismiss = std::get_if<composition::DismissSurfaceIntent>(&intent);
        return dismiss && dismiss->family == SurfaceFamily::launcher;
    }));

    ASSERT_TRUE(runtime.dismiss(SurfaceFamily::actionMenu));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    const auto &dismissBatch = port.batches.back();
    const auto restore = std::ranges::find_if(dismissBatch, [](const auto &intent) {
        return std::holds_alternative<composition::RestoreControlFocusIntent>(intent);
    });
    ASSERT_NE(restore, dismissBatch.end());
    EXPECT_EQ(std::get<composition::RestoreControlFocusIntent>(*restore).origin, origin);

    {
        PresentationPort taskPort;
        ShellRuntimeComposition taskRuntime{taskPort};
        TopologyHarness taskTopology;

        ASSERT_TRUE(taskRuntime.observeOutputs(taskTopology.observe(1U)));
        ASSERT_TRUE(taskRuntime.observeSettingsOwner(authority(10U, 1U)));
        auto taskControlId = composition::FocusControlId::create("panel-task-strip").value();
        auto taskOrigin = composition::FocusOrigin::control(
                              output, composition::FocusControlRole::panelTaskGroup, taskControlId)
                              .value();

        ASSERT_TRUE(taskRuntime.present(SurfaceFamily::actionMenu, "LEFT-1", taskOrigin));
        ASSERT_TRUE(taskRuntime.dismissToWindowManager(SurfaceFamily::actionMenu));
        ASSERT_TRUE(taskRuntime.state().activeSurfaces().empty());
        const auto &taskDismissBatch = taskPort.batches.back();
        EXPECT_TRUE(std::ranges::any_of(taskDismissBatch, [](const auto &intent) {
            return std::holds_alternative<composition::ReleaseFocusToWindowManagerIntent>(intent);
        }));
        EXPECT_FALSE(std::ranges::any_of(taskDismissBatch, [](const auto &intent) {
            return std::holds_alternative<composition::RestoreControlFocusIntent>(intent);
        }));
    }
}

TEST(ShellRuntimeCompositionTest, PanelLauncherTogglePreservesExactEscapeFocusOrigin) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    auto output = composition::OutputName::create("LEFT-1").value();
    auto controlId = composition::FocusControlId::create("launcher-button").value();
    auto origin = composition::FocusOrigin::control(
                      output, composition::FocusControlRole::panelLauncher, controlId)
                      .value();

    ASSERT_TRUE(runtime.toggle(SurfaceFamily::launcher, "LEFT-1", origin));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().origin, origin);

    ASSERT_TRUE(runtime.escape());
    ASSERT_TRUE(runtime.state().activeSurfaces().empty());
    const auto &escapeBatch = port.batches.back();
    const auto restored = std::ranges::find_if(escapeBatch, [](const auto &intent) {
        return std::holds_alternative<composition::RestoreControlFocusIntent>(intent);
    });
    ASSERT_NE(restored, escapeBatch.end());
    EXPECT_EQ(std::get<composition::RestoreControlFocusIntent>(*restored).origin, origin);
    EXPECT_FALSE(std::ranges::any_of(escapeBatch, [](const auto &intent) {
        return std::holds_alternative<composition::ReleaseFocusToWindowManagerIntent>(intent);
    }));
}

TEST(ShellRuntimeCompositionTest, NotificationCenterDismissRestoresExactPanelControlOrigin) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_TRUE(runtime.observeNotificationOwner(authority(20U, 1U)));
    auto output = composition::OutputName::create("LEFT-1").value();
    auto controlId = composition::FocusControlId::create("notification-center-button").value();
    auto origin = composition::FocusOrigin::control(
                      output, composition::FocusControlRole::panelNotificationCenter, controlId)
                      .value();

    ASSERT_TRUE(runtime.present(SurfaceFamily::notificationCenter, "LEFT-1", origin));
    ASSERT_TRUE(runtime.dismiss(SurfaceFamily::notificationCenter));
    ASSERT_TRUE(runtime.state().activeSurfaces().empty());
    const auto &dismissBatch = port.batches.back();
    const auto restored = std::ranges::find_if(dismissBatch, [](const auto &intent) {
        return std::holds_alternative<composition::RestoreControlFocusIntent>(intent);
    });
    ASSERT_NE(restored, dismissBatch.end());
    EXPECT_EQ(std::get<composition::RestoreControlFocusIntent>(*restored).origin, origin);
    EXPECT_FALSE(std::ranges::any_of(dismissBatch, [](const auto &intent) {
        return std::holds_alternative<composition::ReleaseFocusToWindowManagerIntent>(intent);
    }));
}

TEST(ShellRuntimeCompositionTest, LauncherMenuBlocksLowerPrioritySiblingUntilLogoutPreemptsStack) {
    PresentationPort port;
    ShellRuntimeComposition runtime{port};
    TopologyHarness topology;

    ASSERT_TRUE(runtime.observeOutputs(topology.observe(1U)));
    ASSERT_TRUE(runtime.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_TRUE(runtime.observeNotificationOwner(authority(20U, 1U)));
    ASSERT_EQ(runtime.request(control::ShellAction::toggle_launcher, "LEFT-1"),
              ShellRequestFailure::none);
    auto output = composition::OutputName::create("LEFT-1").value();
    auto controlId = composition::FocusControlId::create("launcher-result").value();
    auto origin = composition::FocusOrigin::control(
                      output, composition::FocusControlRole::launcherResult, controlId)
                      .value();
    ASSERT_TRUE(runtime.present(SurfaceFamily::actionMenu, "LEFT-1", origin));

    EXPECT_EQ(runtime.request(control::ShellAction::open_quick_settings, "LEFT-1"),
              ShellRequestFailure::busy);
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 2U);
    EXPECT_NE(std::ranges::find(runtime.state().activeSurfaces(), SurfaceFamily::launcher,
                                &composition::ActiveSurface::family),
              runtime.state().activeSurfaces().end());
    EXPECT_NE(std::ranges::find(runtime.state().activeSurfaces(), SurfaceFamily::actionMenu,
                                &composition::ActiveSurface::family),
              runtime.state().activeSurfaces().end());
    EXPECT_TRUE(
        std::holds_alternative<composition::FocusSurfaceIntent>(port.batches.back().front()));
    EXPECT_EQ(std::get<composition::FocusSurfaceIntent>(port.batches.back().front()).family,
              SurfaceFamily::actionMenu);

    ASSERT_TRUE(runtime.present(SurfaceFamily::logoutConfirmation, "LEFT-1",
                                composition::FocusOrigin::windowManager()));
    ASSERT_EQ(runtime.state().activeSurfaces().size(), 1U);
    EXPECT_EQ(runtime.state().activeSurfaces().front().family, SurfaceFamily::logoutConfirmation);
    const auto &logoutBatch = port.batches.back();
    EXPECT_TRUE(std::ranges::any_of(logoutBatch, [](const auto &intent) {
        const auto *dismiss = std::get_if<composition::DismissSurfaceIntent>(&intent);
        return dismiss && dismiss->family == SurfaceFamily::launcher;
    }));
    EXPECT_TRUE(std::ranges::any_of(logoutBatch, [](const auto &intent) {
        const auto *dismiss = std::get_if<composition::DismissSurfaceIntent>(&intent);
        return dismiss && dismiss->family == SurfaceFamily::actionMenu;
    }));
}

} // namespace
} // namespace prismdrake::shell::runtime
