#pragma once

#include "PanelStrutGeometry.hpp"
#include "Result.hpp"
#include "ScreenReconciliation.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::shell::outputs {

enum class PanelOutputPolicy : std::uint8_t {
    primary,
    all,
    named,
};

struct PanelPlacementPolicy final {
    PanelOutputPolicy policy;
    std::vector<std::string> outputNames;

    friend bool operator==(const PanelPlacementPolicy &, const PanelPlacementPolicy &) = default;
};

enum class SurfaceKind : std::uint8_t {
    desktop,
    panel,
    launcher,
    transientNotifications,
    notificationCenter,
    quickSettings,
    logoutConfirmation,
};

enum class TopologyEventKind : std::uint8_t {
    added,
    removed,
    geometryChanged,
    primaryChanged,
    scaleChanged,
};

struct TopologyEvent final {
    TopologyEventKind kind;
    std::string outputName;

    friend bool operator==(const TopologyEvent &, const TopologyEvent &) = default;
};

struct SurfacePlacement final {
    SurfaceKind kind;
    std::string outputName;
    LogicalRect logicalGeometry;
    std::uint64_t topologyGeneration;
    std::optional<x11::BottomPanelStrut> panelStrut;
};

class SurfaceRegistrySnapshot final {
  public:
    [[nodiscard]] std::uint64_t topologyGeneration() const noexcept { return topology_generation_; }
    [[nodiscard]] std::uint64_t settingsGeneration() const noexcept { return settings_generation_; }
    [[nodiscard]] std::uint64_t surfaceGeneration() const noexcept { return surface_generation_; }
    [[nodiscard]] const std::optional<std::string> &primaryOutputName() const noexcept {
        return primary_output_name_;
    }
    [[nodiscard]] std::span<const SurfacePlacement> desktops() const noexcept { return desktops_; }
    [[nodiscard]] std::span<const SurfacePlacement> panels() const noexcept { return panels_; }
    [[nodiscard]] std::span<const SurfacePlacement> transients() const noexcept {
        return transients_;
    }
    [[nodiscard]] std::span<const TopologyEvent> events() const noexcept { return events_; }

  private:
    friend class OutputSurfaceRegistry;

    std::uint64_t topology_generation_{0U};
    std::uint64_t settings_generation_{0U};
    std::uint64_t surface_generation_{0U};
    std::optional<std::string> primary_output_name_;
    std::vector<SurfacePlacement> desktops_;
    std::vector<SurfacePlacement> panels_;
    std::vector<SurfacePlacement> transients_;
    std::vector<TopologyEvent> events_;
};

/// Owns shell-only placements. It never chooses WM focus, stacking, work-area,
/// or compositor policy, and publishes only complete topology generations.
class OutputSurfaceRegistry final {
  public:
    [[nodiscard]] static foundation::Result<OutputSurfaceRegistry>
    create(std::uint32_t panelHeightLogical);

    [[nodiscard]] foundation::Result<SurfaceRegistrySnapshot>
    observe(const ReconciledTopologySnapshot &topology, const PanelPlacementPolicy &panelPolicy);
    [[nodiscard]] foundation::Result<SurfaceRegistrySnapshot>
    observe(const ReconciledTopologySnapshot &topology, const PanelPlacementPolicy &panelPolicy,
            std::uint64_t settingsGeneration);
    [[nodiscard]] foundation::Result<SurfaceRegistrySnapshot>
    reconfigure(std::uint32_t panelHeightLogical, const ReconciledTopologySnapshot &topology,
                const PanelPlacementPolicy &panelPolicy, std::uint64_t settingsGeneration);

    [[nodiscard]] foundation::Result<SurfacePlacement>
    anchorTransient(SurfaceKind kind, std::string_view invokingOutputName = {});
    void dismissTransient(SurfaceKind kind) noexcept;

    [[nodiscard]] const std::optional<SurfaceRegistrySnapshot> &current() const noexcept {
        return current_;
    }

  private:
    struct TransientIntent final {
        SurfaceKind kind;
        std::string outputName;
    };

    explicit OutputSurfaceRegistry(std::uint32_t panelHeightLogical) noexcept
        : panel_height_logical_(panelHeightLogical) {}

    [[nodiscard]] foundation::Result<SurfacePlacement>
    resolveTransient(SurfaceKind kind, std::string_view requestedOutput) const;

    std::uint32_t panel_height_logical_;
    std::optional<ReconciledTopologySnapshot> topology_;
    std::optional<PanelPlacementPolicy> panel_policy_;
    std::uint64_t settings_generation_{0U};
    std::uint64_t surface_generation_{0U};
    std::optional<SurfaceRegistrySnapshot> current_;
    std::vector<TransientIntent> transient_intents_;
};

} // namespace prismdrake::shell::outputs
