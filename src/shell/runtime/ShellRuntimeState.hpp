#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace prismdrake::shell::runtime {

enum class RuntimeAction : std::uint8_t {
    createPresentationEpoch,
    updatePresentationEpoch,
    destroyPresentationEpoch,
    showLauncher,
    hideLauncher,
    requestLauncherFocus,
    requestPanelKeyboardAccess,
    releasePanelKeyboardAccess,
    requestPanelLauncherFocus,
    requestShutdown,
};

/// Display-free lifecycle policy for one shell process.
///
/// Settings-owner epochs own only theme-backed presentation surfaces. Authoritative X11 task
/// state remains in the task controller, and any X11 connection loss terminates the process so a
/// supervisor can rebuild every X11 observation from scratch.
class ShellRuntimeState final {
  public:
    [[nodiscard]] std::vector<RuntimeAction> acceptSettingsSnapshot();
    [[nodiscard]] std::vector<RuntimeAction> loseSettingsOwner();
    [[nodiscard]] std::vector<RuntimeAction> openLauncher();
    [[nodiscard]] std::vector<RuntimeAction> dismissLauncher(bool returnFocusToPanel);
    [[nodiscard]] std::vector<RuntimeAction> leavePanelKeyboardNavigation();
    [[nodiscard]] std::vector<RuntimeAction> loseX11Connection();

    [[nodiscard]] bool presentationAvailable() const noexcept { return presentation_available_; }
    [[nodiscard]] bool launcherVisible() const noexcept { return launcher_visible_; }
    [[nodiscard]] bool stopping() const noexcept { return stopping_; }

  private:
    bool presentation_available_{false};
    bool launcher_visible_{false};
    bool stopping_{false};
};

/// Complete production publication observed before the shell tells its session supervisor that
/// startup succeeded. Generation equality prevents a partly applied settings or output update
/// from escaping through the process-readiness boundary.
struct ShellReadinessObservation final {
    bool processStarted{false};
    bool settingsOwnerAvailable{false};
    bool presentationSurfacesReady{false};
    bool hasPrimaryOutput{false};
    std::uint64_t settingsGeneration{0U};
    std::uint64_t outputSettingsGeneration{0U};
    std::uint64_t outputSurfaceGeneration{0U};
    std::uint64_t panelSurfaceGeneration{0U};
    std::uint64_t outputTopologyGeneration{0U};
    std::uint64_t desktopTopologyGeneration{0U};
    std::uint64_t wallpaperSettingsGeneration{0U};
    std::size_t outputDesktopCount{0U};
    std::size_t desktopSurfaceCount{0U};
};

/// One-shot readiness gate for the real shell composition root.
class ShellReadinessGate final {
  public:
    /// Returns true exactly once, after one coherent production publication is complete.
    [[nodiscard]] bool consumeIfComplete(const ShellReadinessObservation &observation) noexcept;
    [[nodiscard]] bool consumed() const noexcept { return consumed_; }

  private:
    bool consumed_{false};
};

} // namespace prismdrake::shell::runtime
