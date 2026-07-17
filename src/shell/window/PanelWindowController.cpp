#include "PanelWindowController.hpp"

#include <limits>
#include <utility>

namespace prismdrake::shell::window {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error invalidPanelHeight() {
    return {ErrorCode::invalid_argument, "The panel height is invalid.",
            "Use a nonzero panel height representable by Qt window geometry."};
}

[[nodiscard]] foundation::Error unsupportedWindowGeometry() {
    return {ErrorCode::unsupported, "The selected panel geometry is not representable by Qt.",
            "Retain the previous placement and select an output within signed window bounds."};
}

[[nodiscard]] bool fitsQtCoordinate(std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
}

} // namespace

Result<PanelWindowController> PanelWindowController::create(std::uint32_t panelHeight) {
    if (panelHeight == 0U || !fitsQtCoordinate(panelHeight)) {
        return Result<PanelWindowController>::failure(invalidPanelHeight());
    }
    return Result<PanelWindowController>::success(PanelWindowController{panelHeight});
}

Result<PanelWindowPlacement>
PanelWindowController::observe(const x11::RandrTopologySnapshot &snapshot) {
    auto update = prepare(snapshot);
    if (!update) {
        return Result<PanelWindowPlacement>::failure(update.error());
    }
    auto placement = update.value().placement();
    commit(std::move(update).value());
    return Result<PanelWindowPlacement>::success(std::move(placement));
}

Result<PanelWindowUpdate>
PanelWindowController::prepare(const x11::RandrTopologySnapshot &snapshot) const {
    return prepare(snapshot, panel_height_);
}

Result<PanelWindowUpdate> PanelWindowController::prepare(const x11::RandrTopologySnapshot &snapshot,
                                                         std::uint32_t panelHeight) const {
    if (panelHeight == 0U || !fitsQtCoordinate(panelHeight)) {
        return Result<PanelWindowUpdate>::failure(invalidPanelHeight());
    }

    auto root = x11::RootGeometry::fromScreenInfo(snapshot.coreScreen);
    if (!root) {
        return Result<PanelWindowUpdate>::failure(root.error());
    }

    auto topology = x11::buildOutputTopology(x11::OutputTopologyObservation{
        snapshot.randrAvailable(), root.value(), snapshot.resourceOutputCount,
        snapshot.resourceCrtcCount, snapshot.resourceModeCount, snapshot.activeOutputs,
        snapshot.primary});
    if (!topology) {
        return Result<PanelWindowUpdate>::failure(topology.error());
    }

    const auto &selection = topology.value().selection();
    auto dock = x11::calculateBottomPanelStrut(topology.value().coreRoot(), selection.geometry(),
                                               panelHeight);
    if (!dock) {
        return Result<PanelWindowUpdate>::failure(dock.error());
    }

    const auto &panel = dock.value().panel;
    if (!fitsQtCoordinate(panel.x) || !fitsQtCoordinate(panel.y) ||
        !fitsQtCoordinate(panel.width) || !fitsQtCoordinate(panel.height)) {
        return Result<PanelWindowUpdate>::failure(unsupportedWindowGeometry());
    }

    return Result<PanelWindowUpdate>::success(PanelWindowUpdate{
        panelHeight,
        {topology.value().coreRoot(), selection.geometry(), dock.value(), selection.reason()}});
}

void PanelWindowController::commit(PanelWindowUpdate update) noexcept {
    panel_height_ = update.panel_height_;
    current_ = std::move(update.placement_);
}

std::optional<PanelWindowUpdate> PanelWindowController::currentUpdate() const {
    if (!current_) {
        return std::nullopt;
    }
    return PanelWindowUpdate{panel_height_, *current_};
}

} // namespace prismdrake::shell::window
