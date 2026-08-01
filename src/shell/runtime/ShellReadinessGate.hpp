#pragma once

#include <cstddef>
#include <cstdint>

namespace prismdrake::shell::runtime {

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
