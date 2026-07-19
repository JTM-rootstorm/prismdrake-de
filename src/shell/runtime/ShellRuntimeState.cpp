#include "ShellRuntimeState.hpp"

namespace prismdrake::shell::runtime {

std::vector<RuntimeAction> ShellRuntimeState::acceptSettingsSnapshot() {
    if (stopping_) {
        return {};
    }
    if (!presentation_available_) {
        presentation_available_ = true;
        return {RuntimeAction::createPresentationEpoch};
    }
    return {RuntimeAction::updatePresentationEpoch};
}

std::vector<RuntimeAction> ShellRuntimeState::loseSettingsOwner() {
    if (stopping_ || !presentation_available_) {
        return {};
    }
    std::vector<RuntimeAction> actions;
    if (launcher_visible_) {
        actions.push_back(RuntimeAction::hideLauncher);
    }
    actions.push_back(RuntimeAction::releasePanelKeyboardAccess);
    actions.push_back(RuntimeAction::destroyPresentationEpoch);
    launcher_visible_ = false;
    presentation_available_ = false;
    return actions;
}

std::vector<RuntimeAction> ShellRuntimeState::openLauncher() {
    if (stopping_ || !presentation_available_ || launcher_visible_) {
        return {};
    }
    launcher_visible_ = true;
    return {RuntimeAction::releasePanelKeyboardAccess, RuntimeAction::showLauncher,
            RuntimeAction::requestLauncherFocus};
}

std::vector<RuntimeAction> ShellRuntimeState::dismissLauncher(bool returnFocusToPanel) {
    if (stopping_ || !launcher_visible_) {
        return {};
    }
    launcher_visible_ = false;
    if (!returnFocusToPanel || !presentation_available_) {
        return {RuntimeAction::hideLauncher, RuntimeAction::releasePanelKeyboardAccess};
    }
    return {RuntimeAction::hideLauncher, RuntimeAction::requestPanelKeyboardAccess,
            RuntimeAction::requestPanelLauncherFocus};
}

std::vector<RuntimeAction> ShellRuntimeState::leavePanelKeyboardNavigation() {
    if (stopping_ || !presentation_available_) {
        return {};
    }
    return {RuntimeAction::releasePanelKeyboardAccess};
}

std::vector<RuntimeAction> ShellRuntimeState::loseX11Connection() {
    if (stopping_) {
        return {};
    }
    stopping_ = true;
    launcher_visible_ = false;
    return {RuntimeAction::hideLauncher, RuntimeAction::releasePanelKeyboardAccess,
            RuntimeAction::requestShutdown};
}

bool ShellReadinessGate::consumeIfComplete(const ShellReadinessObservation &observation) noexcept {
    if (consumed_) {
        return false;
    }
    const bool complete =
        observation.processStarted && observation.settingsOwnerAvailable &&
        observation.presentationSurfacesReady && observation.hasPrimaryOutput &&
        observation.settingsGeneration != 0U && observation.outputSurfaceGeneration != 0U &&
        observation.outputTopologyGeneration != 0U && observation.outputDesktopCount != 0U &&
        observation.settingsGeneration == observation.outputSettingsGeneration &&
        observation.settingsGeneration == observation.wallpaperSettingsGeneration &&
        observation.outputSurfaceGeneration == observation.panelSurfaceGeneration &&
        observation.outputTopologyGeneration == observation.desktopTopologyGeneration &&
        observation.outputDesktopCount == observation.desktopSurfaceCount;
    if (complete) {
        consumed_ = true;
    }
    return complete;
}

} // namespace prismdrake::shell::runtime
