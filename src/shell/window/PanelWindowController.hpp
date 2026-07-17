#pragma once

#include "OutputTopology.hpp"
#include "PanelStrutGeometry.hpp"
#include "RandrTopology.hpp"
#include "Result.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace prismdrake::shell::window {

/// One validated, toolkit-representable placement for the PD1 bottom panel.
///
/// The selected output and its reason come from the shared X11 topology policy;
/// the dock reservation comes from the shared bottom-strut calculation.
struct PanelWindowPlacement final {
    x11::RootGeometry root;
    x11::OutputGeometry output;
    x11::BottomPanelStrut dock;
    x11::OutputSelectionReason selectionReason;

    friend bool operator==(const PanelWindowPlacement &, const PanelWindowPlacement &) = default;
};

/// One fully validated candidate that is not authoritative until commit() succeeds after the
/// X11 host has published matching dock state.
class PanelWindowUpdate final {
  public:
    [[nodiscard]] std::uint32_t panelHeight() const noexcept { return panel_height_; }
    [[nodiscard]] const PanelWindowPlacement &placement() const noexcept { return placement_; }

    friend bool operator==(const PanelWindowUpdate &, const PanelWindowUpdate &) = default;

  private:
    friend class PanelWindowController;

    PanelWindowUpdate(std::uint32_t panelHeight, PanelWindowPlacement placement) noexcept
        : panel_height_(panelHeight), placement_(std::move(placement)) {}

    std::uint32_t panel_height_;
    PanelWindowPlacement placement_;
};

/// Display-free state boundary between complete RandR observations and a panel window host.
///
/// A failed observation never replaces current(). This keeps the previously validated panel
/// placement intact while the event-driven host waits for the next complete topology refresh.
class PanelWindowController final {
  public:
    [[nodiscard]] static foundation::Result<PanelWindowController>
    create(std::uint32_t panelHeight);

    [[nodiscard]] foundation::Result<PanelWindowPlacement>
    observe(const x11::RandrTopologySnapshot &snapshot);

    /// Validates a topology refresh at the current committed height without changing current().
    [[nodiscard]] foundation::Result<PanelWindowUpdate>
    prepare(const x11::RandrTopologySnapshot &snapshot) const;

    /// Validates a topology and runtime height update without changing current().
    [[nodiscard]] foundation::Result<PanelWindowUpdate>
    prepare(const x11::RandrTopologySnapshot &snapshot, std::uint32_t panelHeight) const;

    /// Publishes a candidate only after the host has applied matching X11 and window state.
    void commit(PanelWindowUpdate update) noexcept;

    [[nodiscard]] std::uint32_t panelHeight() const noexcept { return panel_height_; }
    [[nodiscard]] const std::optional<PanelWindowPlacement> &current() const noexcept {
        return current_;
    }
    [[nodiscard]] std::optional<PanelWindowUpdate> currentUpdate() const;

  private:
    explicit PanelWindowController(std::uint32_t panelHeight) noexcept
        : panel_height_(panelHeight) {}

    std::uint32_t panel_height_;
    std::optional<PanelWindowPlacement> current_;
};

} // namespace prismdrake::shell::window
