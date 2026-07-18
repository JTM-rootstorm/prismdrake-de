#pragma once

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

} // namespace prismdrake::shell::runtime
