#include "ShellReadinessGate.hpp"

namespace prismdrake::shell::runtime {

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
