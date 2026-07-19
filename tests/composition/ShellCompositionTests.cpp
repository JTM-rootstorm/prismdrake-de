#include "ShellComposition.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::shell::composition {
namespace {

[[nodiscard]] OutputName output(std::string value) {
    return OutputName::create(std::move(value)).value();
}

[[nodiscard]] FocusControlId controlId(std::string value) {
    return FocusControlId::create(std::move(value)).value();
}

[[nodiscard]] FocusOrigin control(std::string outputName, FocusControlRole role, std::string id) {
    return FocusOrigin::control(output(std::move(outputName)), role, controlId(std::move(id)))
        .value();
}

[[nodiscard]] AuthoritySnapshot authority(std::uint64_t epoch, std::uint64_t generation) {
    return {OwnerEpoch::create(epoch).value(), OwnerGeneration::create(generation).value()};
}

[[nodiscard]] OutputPlacementSnapshot placements(std::uint64_t generation,
                                                 std::vector<std::pair<std::string, bool>> names,
                                                 std::optional<std::string> primary) {
    std::vector<OutputPlacement> outputs;
    outputs.reserve(names.size());
    for (auto &[name, panel] : names) {
        outputs.push_back({output(std::move(name)), panel});
    }
    std::optional<OutputName> primaryOutput;
    if (primary) {
        primaryOutput = output(std::move(*primary));
    }
    return OutputPlacementSnapshot::create(generation, std::move(outputs), std::move(primaryOutput))
        .value();
}

void ready(ShellComposition &composition) {
    ASSERT_TRUE(
        composition.observeOutputs(placements(1U, {{"HDMI-1", true}, {"DP-1", true}}, "HDMI-1")));
    ASSERT_TRUE(composition.observeSettingsOwner(authority(10U, 1U)));
    ASSERT_TRUE(composition.observeNotificationOwner(authority(20U, 1U)));
}

template <typename Intent>
[[nodiscard]] std::vector<const Intent *> intents(const CompositionTransition &transition) {
    std::vector<const Intent *> matches;
    for (const auto &intent : transition.intents()) {
        if (const auto *match = std::get_if<Intent>(&intent); match != nullptr) {
            matches.push_back(match);
        }
    }
    return matches;
}

TEST(OutputPlacementSnapshotTest, RejectsInvalidAndDuplicateCompletePlacements) {
    EXPECT_FALSE(OutputName::create("bad\nname"));
    EXPECT_FALSE(FocusControlId::create(""));
    EXPECT_FALSE(OwnerEpoch::create(0U));
    EXPECT_FALSE(OwnerGeneration::create(0U));

    EXPECT_FALSE(OutputPlacementSnapshot::create(0U, {}, std::nullopt));
    EXPECT_FALSE(OutputPlacementSnapshot::create(
        1U, {{output("DP-1"), true}, {output("DP-1"), false}}, output("DP-1")));
    EXPECT_FALSE(OutputPlacementSnapshot::create(1U, {{output("DP-1"), true}}, output("HDMI-1")));
}

TEST(ShellCompositionTest, RejectsStaleTopologyAndAuthorityGenerationsAtomically) {
    ShellComposition composition;
    ready(composition);

    EXPECT_FALSE(
        composition.observeOutputs(placements(1U, {{"HDMI-1", true}, {"DP-1", true}}, "DP-1")));
    EXPECT_FALSE(composition.observeSettingsOwner(authority(10U, 1U)));
    EXPECT_FALSE(composition.observeNotificationOwner(authority(20U, 1U)));
    EXPECT_TRUE(composition.activeSurfaces().empty());

    EXPECT_TRUE(composition.observeSettingsOwner(authority(10U, 2U)));
    EXPECT_TRUE(composition.observeNotificationOwner(authority(20U, 2U)));
}

TEST(ShellCompositionTest, MovesGlobalSurfaceAndFocusOriginAfterOutputRemoval) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("DP-1"),
                            control("DP-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto update = composition.observeOutputs(placements(2U, {{"HDMI-1", true}}, "HDMI-1"));

    ASSERT_TRUE(update);
    const auto moves = intents<MoveSurfaceIntent>(update.value());
    ASSERT_EQ(moves.size(), 1U);
    EXPECT_EQ(moves[0]->family, SurfaceFamily::launcher);
    EXPECT_EQ(moves[0]->from.value(), "DP-1");
    EXPECT_EQ(moves[0]->to.value(), "HDMI-1");
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
    EXPECT_EQ(composition.activeSurfaces()[0].output.value(), "HDMI-1");
    EXPECT_TRUE(composition.activeSurfaces()[0].origin.equivalent());
    ASSERT_TRUE(composition.activeSurfaces()[0].origin.output());
    EXPECT_EQ(composition.activeSurfaces()[0].origin.output()->value(), "HDMI-1");

    const auto escaped = composition.escape();
    const auto restores = intents<RestoreControlFocusIntent>(escaped);
    ASSERT_EQ(restores.size(), 1U);
    EXPECT_TRUE(restores[0]->origin.equivalent());
    EXPECT_EQ(restores[0]->origin.output()->value(), "HDMI-1");
}

TEST(ShellCompositionTest, ReleasesFocusWhenNoOutputFallbackExists) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("DP-1"),
                            control("DP-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto update = composition.observeOutputs(placements(2U, {}, std::nullopt));

    ASSERT_TRUE(update);
    EXPECT_EQ(intents<DismissSurfaceIntent>(update.value()).size(), 1U);
    EXPECT_EQ(intents<ReleaseFocusToWindowManagerIntent>(update.value()).size(), 1U);
    EXPECT_TRUE(composition.activeSurfaces().empty());
    EXPECT_FALSE(composition.focusedFamily());
}

TEST(ShellCompositionTest, TransfersSingletonWithNewInvocationOrigin) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(composition.present(
        SurfaceFamily::notificationCenter, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelNotificationCenter, "notification-button")));

    const auto moved = composition.present(
        SurfaceFamily::notificationCenter, output("DP-1"),
        control("DP-1", FocusControlRole::panelNotificationCenter, "notification-button"));

    ASSERT_TRUE(moved);
    EXPECT_EQ(intents<MoveSurfaceIntent>(moved.value()).size(), 1U);
    EXPECT_EQ(intents<PresentSurfaceIntent>(moved.value()).size(), 0U);
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
    EXPECT_EQ(composition.activeSurfaces()[0].output.value(), "DP-1");
    EXPECT_FALSE(composition.activeSurfaces()[0].origin.equivalent());
}

TEST(ShellCompositionTest, DismissesConflictsInAcceptedPriorityOrder) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                            control("HDMI-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto quick = composition.present(
        SurfaceFamily::quickSettings, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelQuickSettings, "quick-button"));
    ASSERT_TRUE(quick);
    ASSERT_EQ(intents<DismissSurfaceIntent>(quick.value()).size(), 1U);
    EXPECT_EQ(intents<DismissSurfaceIntent>(quick.value())[0]->family, SurfaceFamily::launcher);

    const auto action =
        composition.present(SurfaceFamily::actionMenu, output("HDMI-1"),
                            control("HDMI-1", FocusControlRole::panelTaskGroup, "task-group"));
    ASSERT_TRUE(action);
    EXPECT_EQ(intents<DismissSurfaceIntent>(action.value())[0]->family,
              SurfaceFamily::quickSettings);

    const auto blocked = composition.present(
        SurfaceFamily::notificationCenter, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelNotificationCenter, "notification-button"));
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked.value().disposition(), TransitionDisposition::blockedByHigherPriority);
    EXPECT_EQ(intents<FocusSurfaceIntent>(blocked.value())[0]->family, SurfaceFamily::actionMenu);
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
}

TEST(ShellCompositionTest, LogoutIsGlobalModalAndEscapeRequestsSessionCancellation) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("DP-1"),
                            control("DP-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto modal = composition.present(SurfaceFamily::logoutConfirmation, output("DP-1"),
                                           FocusOrigin::windowManager());
    ASSERT_TRUE(modal);
    EXPECT_EQ(intents<DismissSurfaceIntent>(modal.value()).size(), 1U);
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
    EXPECT_EQ(composition.activeSurfaces()[0].family, SurfaceFamily::logoutConfirmation);
    EXPECT_EQ(composition.activeSurfaces()[0].output.value(), "HDMI-1");

    const auto blocked =
        composition.present(SurfaceFamily::launcher, output("DP-1"),
                            control("DP-1", FocusControlRole::panelLauncher, "launcher-button"));
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked.value().disposition(), TransitionDisposition::blockedByHigherPriority);
    EXPECT_EQ(composition.activeSurfaces().size(), 1U);

    const auto escaped = composition.escape();
    EXPECT_EQ(intents<CancelLogoutIntent>(escaped).size(), 1U);
    EXPECT_TRUE(intents<DismissSurfaceIntent>(escaped).empty());
    EXPECT_EQ(composition.activeSurfaces().size(), 1U);
}

TEST(ShellCompositionTest, EscapeRestoresExactControlOrReleasesToWindowManager) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                            control("HDMI-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto exact = composition.escape();
    ASSERT_EQ(intents<RestoreControlFocusIntent>(exact).size(), 1U);
    EXPECT_FALSE(intents<RestoreControlFocusIntent>(exact)[0]->origin.equivalent());

    ASSERT_TRUE(composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                                    FocusOrigin::windowManager()));
    const auto released = composition.escape();
    EXPECT_EQ(intents<ReleaseFocusToWindowManagerIntent>(released).size(), 1U);
}

TEST(ShellCompositionTest, NotifyEpochInvalidatesOnlyDependentSurfaces) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(composition.present(
        SurfaceFamily::quickSettings, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelQuickSettings, "quick-button")));
    ASSERT_TRUE(composition.present(
        SurfaceFamily::notificationCenter, output("DP-1"),
        control("DP-1", FocusControlRole::panelNotificationCenter, "notification-button")));

    const auto lost = composition.observeNotificationOwner(std::nullopt);

    ASSERT_TRUE(lost);
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
    EXPECT_EQ(composition.activeSurfaces()[0].family, SurfaceFamily::quickSettings);
    EXPECT_EQ(intents<DismissSurfaceIntent>(lost.value()).size(), 1U);
    const auto dependency = intents<DependencyAvailabilityIntent>(lost.value());
    ASSERT_EQ(dependency.size(), 1U);
    EXPECT_FALSE(dependency[0]->available);

    const auto restored = composition.observeNotificationOwner(authority(21U, 1U));
    ASSERT_TRUE(restored);
    ASSERT_EQ(intents<DependencyAvailabilityIntent>(restored.value()).size(), 1U);
    EXPECT_TRUE(intents<DependencyAvailabilityIntent>(restored.value())[0]->available);
}

TEST(ShellCompositionTest, DirectOwnerEpochChangesInvalidateWithoutMixedAvailability) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(composition.present(
        SurfaceFamily::notificationCenter, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelNotificationCenter, "notification-button")));

    const auto notifyReplaced = composition.observeNotificationOwner(authority(21U, 1U));

    ASSERT_TRUE(notifyReplaced);
    EXPECT_TRUE(composition.activeSurfaces().empty());
    EXPECT_EQ(intents<DismissSurfaceIntent>(notifyReplaced.value()).size(), 1U);
    const auto dependency = intents<DependencyAvailabilityIntent>(notifyReplaced.value());
    ASSERT_EQ(dependency.size(), 1U);
    EXPECT_TRUE(dependency[0]->available);

    ASSERT_TRUE(composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                                    FocusOrigin::windowManager()));
    const auto settingsReplaced = composition.observeSettingsOwner(authority(11U, 1U));
    ASSERT_TRUE(settingsReplaced);
    EXPECT_TRUE(composition.activeSurfaces().empty());
    EXPECT_EQ(intents<DismissSurfaceIntent>(settingsReplaced.value()).size(), 1U);
}

TEST(ShellCompositionTest, OutputFallbackWithoutPanelUsesWindowManagerRecovery) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(
        composition.present(SurfaceFamily::launcher, output("DP-1"),
                            control("DP-1", FocusControlRole::panelLauncher, "launcher-button")));

    const auto update = composition.observeOutputs(placements(2U, {{"HDMI-1", false}}, "HDMI-1"));
    ASSERT_TRUE(update);
    ASSERT_EQ(composition.activeSurfaces().size(), 1U);
    EXPECT_EQ(composition.activeSurfaces()[0].origin.kind(), FocusOriginKind::windowManager);

    const auto escaped = composition.escape();
    EXPECT_EQ(intents<ReleaseFocusToWindowManagerIntent>(escaped).size(), 1U);
}

TEST(ShellCompositionTest, SettingsEpochLossInvalidatesAllPresentation) {
    ShellComposition composition;
    ready(composition);
    ASSERT_TRUE(composition.present(
        SurfaceFamily::quickSettings, output("HDMI-1"),
        control("HDMI-1", FocusControlRole::panelQuickSettings, "quick-button")));

    const auto lost = composition.observeSettingsOwner(std::nullopt);

    ASSERT_TRUE(lost);
    EXPECT_TRUE(composition.activeSurfaces().empty());
    EXPECT_EQ(intents<DismissSurfaceIntent>(lost.value()).size(), 1U);
    EXPECT_EQ(intents<ReleaseFocusToWindowManagerIntent>(lost.value()).size(), 1U);
    EXPECT_FALSE(composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                                     FocusOrigin::windowManager()));

    ASSERT_TRUE(composition.observeSettingsOwner(authority(11U, 1U)));
    EXPECT_TRUE(composition.present(SurfaceFamily::launcher, output("HDMI-1"),
                                    FocusOrigin::windowManager()));
}

} // namespace
} // namespace prismdrake::shell::composition
