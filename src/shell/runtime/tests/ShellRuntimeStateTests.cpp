#include "ShellRuntimeState.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace prismdrake::shell::runtime {
namespace {

TEST(ShellRuntimeStateTest, RebuildsPresentationAcrossSettingsOwnerEpochs) {
    ShellRuntimeState state;

    EXPECT_EQ(state.acceptSettingsSnapshot(), std::vector{RuntimeAction::createPresentationEpoch});
    EXPECT_TRUE(state.presentationAvailable());
    EXPECT_EQ(state.acceptSettingsSnapshot(), std::vector{RuntimeAction::updatePresentationEpoch});

    EXPECT_EQ(state.loseSettingsOwner(), (std::vector{RuntimeAction::releasePanelKeyboardAccess,
                                                      RuntimeAction::destroyPresentationEpoch}));
    EXPECT_FALSE(state.presentationAvailable());
    EXPECT_EQ(state.acceptSettingsSnapshot(), std::vector{RuntimeAction::createPresentationEpoch});
}

TEST(ShellRuntimeStateTest, LauncherFocusTransferIsExplicitAndBounded) {
    ShellRuntimeState state;
    static_cast<void>(state.acceptSettingsSnapshot());

    EXPECT_EQ(state.openLauncher(),
              (std::vector{RuntimeAction::releasePanelKeyboardAccess, RuntimeAction::showLauncher,
                           RuntimeAction::requestLauncherFocus}));
    EXPECT_TRUE(state.launcherVisible());
    EXPECT_TRUE(state.openLauncher().empty());

    EXPECT_EQ(state.dismissLauncher(true),
              (std::vector{RuntimeAction::hideLauncher, RuntimeAction::requestPanelKeyboardAccess,
                           RuntimeAction::requestPanelLauncherFocus}));
    EXPECT_FALSE(state.launcherVisible());
    EXPECT_EQ(state.leavePanelKeyboardNavigation(),
              std::vector{RuntimeAction::releasePanelKeyboardAccess});

    static_cast<void>(state.openLauncher());
    EXPECT_EQ(
        state.dismissLauncher(false),
        (std::vector{RuntimeAction::hideLauncher, RuntimeAction::releasePanelKeyboardAccess}));
}

TEST(ShellRuntimeStateTest, SettingsLossDismissesLauncherBeforeDestroyingPresentation) {
    ShellRuntimeState state;
    static_cast<void>(state.acceptSettingsSnapshot());
    static_cast<void>(state.openLauncher());

    EXPECT_EQ(state.loseSettingsOwner(),
              (std::vector{RuntimeAction::hideLauncher, RuntimeAction::releasePanelKeyboardAccess,
                           RuntimeAction::destroyPresentationEpoch}));
    EXPECT_FALSE(state.presentationAvailable());
    EXPECT_FALSE(state.launcherVisible());
}

TEST(ShellRuntimeStateTest, X11LossRequestsOneCleanProcessShutdown) {
    ShellRuntimeState state;
    static_cast<void>(state.acceptSettingsSnapshot());
    static_cast<void>(state.openLauncher());

    EXPECT_EQ(state.loseX11Connection(),
              (std::vector{RuntimeAction::hideLauncher, RuntimeAction::releasePanelKeyboardAccess,
                           RuntimeAction::requestShutdown}));
    EXPECT_TRUE(state.stopping());
    EXPECT_TRUE(state.loseX11Connection().empty());
    EXPECT_TRUE(state.acceptSettingsSnapshot().empty());
}

} // namespace
} // namespace prismdrake::shell::runtime
