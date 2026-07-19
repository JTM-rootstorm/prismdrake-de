#include "OutputSurfaceRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace prismdrake::shell::outputs {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidPolicy() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The output surface policy is invalid.",
         "Use a bounded supported panel policy and one complete newer topology generation."});
}

template <typename Value> [[nodiscard]] Result<Value> unavailablePlacement() {
    return Result<Value>::failure(
        {ErrorCode::not_found, "No valid output is available for the shell surface.",
         "Dismiss the surface or wait for a complete reconciled output topology."});
}

[[nodiscard]] bool transientKind(SurfaceKind kind) noexcept {
    return kind == SurfaceKind::launcher || kind == SurfaceKind::transientNotifications ||
           kind == SurfaceKind::notificationCenter || kind == SurfaceKind::quickSettings ||
           kind == SurfaceKind::logoutConfirmation;
}

[[nodiscard]] bool primaryOnly(SurfaceKind kind) noexcept {
    return kind == SurfaceKind::transientNotifications || kind == SurfaceKind::logoutConfirmation;
}

[[nodiscard]] std::vector<const ReconciledOutput *>
availableOutputs(const ReconciledTopologySnapshot &topology) {
    std::vector<const ReconciledOutput *> available;
    for (const auto &output : topology.outputs()) {
        if (output.available()) {
            available.push_back(&output);
        }
    }
    return available;
}

[[nodiscard]] const ReconciledOutput *findOutput(const ReconciledTopologySnapshot &topology,
                                                 std::string_view name) noexcept {
    const auto output =
        std::find_if(topology.outputs().begin(), topology.outputs().end(),
                     [name](const auto &item) { return item.available() && item.name == name; });
    return output == topology.outputs().end() ? nullptr : &*output;
}

[[nodiscard]] SurfacePlacement desktopPlacement(const ReconciledOutput &output,
                                                std::uint64_t generation) {
    return {SurfaceKind::desktop, output.name, *output.logicalGeometry, generation, std::nullopt};
}

[[nodiscard]] Result<SurfacePlacement> panelPlacement(const x11::RootGeometry &root,
                                                      const ReconciledOutput &output,
                                                      std::uint64_t generation,
                                                      std::uint32_t panelHeightLogical) {
    if (panelHeightLogical == 0U || panelHeightLogical > output.logicalGeometry->height) {
        return invalidPolicy<SurfacePlacement>();
    }
    const double nativeHeightValue =
        std::ceil(static_cast<double>(panelHeightLogical) * output.devicePixelRatio);
    if (!std::isfinite(nativeHeightValue) || nativeHeightValue <= 0.0 ||
        nativeHeightValue > std::numeric_limits<std::uint32_t>::max()) {
        return invalidPolicy<SurfacePlacement>();
    }
    auto strut = x11::calculateBottomPanelStrut(root, output.nativeGeometry,
                                                static_cast<std::uint32_t>(nativeHeightValue));
    if (!strut) {
        return Result<SurfacePlacement>::failure(strut.error());
    }
    const auto logical = *output.logicalGeometry;
    return Result<SurfacePlacement>::success(
        {SurfaceKind::panel,
         output.name,
         {logical.x,
          static_cast<std::int32_t>(static_cast<std::int64_t>(logical.y) + logical.height -
                                    panelHeightLogical),
          logical.width, panelHeightLogical},
         generation,
         strut.value()});
}

[[nodiscard]] bool validPolicy(const PanelPlacementPolicy &policy) noexcept {
    if (policy.policy == PanelOutputPolicy::primary || policy.policy == PanelOutputPolicy::all) {
        return true;
    }
    if (policy.policy != PanelOutputPolicy::named) {
        return false;
    }
    if (policy.outputNames.size() > x11::maximumTopologyOutputs) {
        return false;
    }
    for (std::size_t index = 0U; index < policy.outputNames.size(); ++index) {
        if (!x11::isStableOutputName(policy.outputNames[index]) ||
            std::find(policy.outputNames.begin(),
                      policy.outputNames.begin() + static_cast<std::ptrdiff_t>(index),
                      policy.outputNames[index]) !=
                policy.outputNames.begin() + static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<TopologyEvent>
topologyEvents(const std::optional<ReconciledTopologySnapshot> &previous,
               const ReconciledTopologySnapshot &next) {
    std::vector<TopologyEvent> events;
    const auto nextAvailable = availableOutputs(next);
    if (!previous) {
        for (const auto *output : nextAvailable) {
            events.push_back({TopologyEventKind::added, output->name});
        }
        if (next.primaryOutputName()) {
            events.push_back({TopologyEventKind::primaryChanged, *next.primaryOutputName()});
        }
        return events;
    }

    const auto previousAvailable = availableOutputs(*previous);
    for (const auto *oldOutput : previousAvailable) {
        const auto *newOutput = findOutput(next, oldOutput->name);
        if (newOutput == nullptr) {
            events.push_back({TopologyEventKind::removed, oldOutput->name});
            continue;
        }
        if (oldOutput->logicalGeometry != newOutput->logicalGeometry ||
            oldOutput->nativeGeometry != newOutput->nativeGeometry) {
            events.push_back({TopologyEventKind::geometryChanged, oldOutput->name});
        }
        if (oldOutput->devicePixelRatio != newOutput->devicePixelRatio) {
            events.push_back({TopologyEventKind::scaleChanged, oldOutput->name});
        }
    }
    for (const auto *newOutput : nextAvailable) {
        if (findOutput(*previous, newOutput->name) == nullptr) {
            events.push_back({TopologyEventKind::added, newOutput->name});
        }
    }
    if (previous->primaryOutputName() != next.primaryOutputName()) {
        events.push_back(
            {TopologyEventKind::primaryChanged, next.primaryOutputName().value_or(std::string{})});
    }
    return events;
}

} // namespace

Result<OutputSurfaceRegistry> OutputSurfaceRegistry::create(std::uint32_t panelHeightLogical) {
    if (panelHeightLogical == 0U ||
        panelHeightLogical > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return invalidPolicy<OutputSurfaceRegistry>();
    }
    return Result<OutputSurfaceRegistry>::success(OutputSurfaceRegistry{panelHeightLogical});
}

Result<SurfaceRegistrySnapshot>
OutputSurfaceRegistry::observe(const ReconciledTopologySnapshot &topology,
                               const PanelPlacementPolicy &panelPolicy) {
    if (!validPolicy(panelPolicy) ||
        (topology_ && topology.generation() <= topology_->generation())) {
        return invalidPolicy<SurfaceRegistrySnapshot>();
    }

    SurfaceRegistrySnapshot snapshot;
    snapshot.topology_generation_ = topology.generation();
    snapshot.primary_output_name_ = topology.primaryOutputName();
    snapshot.events_ = topologyEvents(topology_, topology);
    const auto available = availableOutputs(topology);
    for (const auto *output : available) {
        snapshot.desktops_.push_back(desktopPlacement(*output, topology.generation()));
    }

    std::vector<const ReconciledOutput *> panelOutputs;
    if (panelPolicy.policy == PanelOutputPolicy::all) {
        panelOutputs = available;
    } else if (panelPolicy.policy == PanelOutputPolicy::named) {
        for (const auto &name : panelPolicy.outputNames) {
            if (const auto *output = findOutput(topology, name); output != nullptr) {
                panelOutputs.push_back(output);
            }
        }
    }
    if ((panelPolicy.policy == PanelOutputPolicy::primary || panelOutputs.empty()) &&
        topology.primaryOutputName()) {
        if (const auto *primary = findOutput(topology, *topology.primaryOutputName());
            primary != nullptr) {
            panelOutputs.push_back(primary);
        }
    }
    for (const auto *output : panelOutputs) {
        auto panel =
            panelPlacement(topology.root(), *output, topology.generation(), panel_height_logical_);
        if (!panel) {
            return Result<SurfaceRegistrySnapshot>::failure(panel.error());
        }
        snapshot.panels_.push_back(std::move(panel).value());
    }

    const auto previousTopology = topology_;
    topology_ = topology;
    for (auto &intent : transient_intents_) {
        auto placement = resolveTransient(intent.kind, intent.outputName);
        if (placement) {
            intent.outputName = placement.value().outputName;
            snapshot.transients_.push_back(std::move(placement).value());
        } else if (!topology.primaryOutputName()) {
            intent.outputName.clear();
        } else {
            topology_ = previousTopology;
            return Result<SurfaceRegistrySnapshot>::failure(placement.error());
        }
    }
    current_ = snapshot;
    return Result<SurfaceRegistrySnapshot>::success(std::move(snapshot));
}

Result<SurfacePlacement>
OutputSurfaceRegistry::resolveTransient(SurfaceKind kind, std::string_view requestedOutput) const {
    if (!transientKind(kind) || !topology_) {
        return unavailablePlacement<SurfacePlacement>();
    }
    const ReconciledOutput *output = nullptr;
    if (!primaryOnly(kind) && !requestedOutput.empty()) {
        output = findOutput(*topology_, requestedOutput);
    }
    if (output == nullptr && topology_->primaryOutputName()) {
        output = findOutput(*topology_, *topology_->primaryOutputName());
    }
    if (output == nullptr) {
        return unavailablePlacement<SurfacePlacement>();
    }
    return Result<SurfacePlacement>::success(
        {kind, output->name, *output->logicalGeometry, topology_->generation(), std::nullopt});
}

Result<SurfacePlacement>
OutputSurfaceRegistry::anchorTransient(SurfaceKind kind, std::string_view invokingOutputName) {
    if (!transientKind(kind) ||
        (!invokingOutputName.empty() && !x11::isStableOutputName(invokingOutputName))) {
        return invalidPolicy<SurfacePlacement>();
    }
    if (!primaryOnly(kind) && !invokingOutputName.empty() &&
        (!topology_ || findOutput(*topology_, invokingOutputName) == nullptr)) {
        return unavailablePlacement<SurfacePlacement>();
    }
    auto placement = resolveTransient(kind, invokingOutputName);
    if (!placement) {
        return placement;
    }
    const auto existing = std::find_if(transient_intents_.begin(), transient_intents_.end(),
                                       [kind](const auto &intent) { return intent.kind == kind; });
    if (existing == transient_intents_.end()) {
        transient_intents_.push_back({kind, placement.value().outputName});
    } else {
        existing->outputName = placement.value().outputName;
    }
    if (current_) {
        auto currentPlacement =
            std::find_if(current_->transients_.begin(), current_->transients_.end(),
                         [kind](const auto &item) { return item.kind == kind; });
        if (currentPlacement == current_->transients_.end()) {
            current_->transients_.push_back(placement.value());
        } else {
            *currentPlacement = placement.value();
        }
    }
    return placement;
}

void OutputSurfaceRegistry::dismissTransient(SurfaceKind kind) noexcept {
    std::erase_if(transient_intents_, [kind](const auto &intent) { return intent.kind == kind; });
    if (current_) {
        std::erase_if(current_->transients_,
                      [kind](const auto &placement) { return placement.kind == kind; });
    }
}

} // namespace prismdrake::shell::outputs
