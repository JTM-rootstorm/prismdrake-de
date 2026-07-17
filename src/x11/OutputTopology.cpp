#include "OutputTopology.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Identifier>
[[nodiscard]] Result<Identifier> invalidIdentifier(const char *identifier) {
    return Result<Identifier>::failure(
        {ErrorCode::invalid_argument,
         std::string{"The X11 "} + identifier + " identifier is invalid.",
         "Use a nonzero identifier obtained from the active X11 connection."});
}

[[nodiscard]] Result<RootGeometry> invalidRoot() {
    return Result<RootGeometry>::failure(
        {ErrorCode::validation_error, "The core X11 root geometry is invalid.",
         "Use nonzero root dimensions representable in protocol coordinates."});
}

[[nodiscard]] Result<OutputGeometry> invalidOutput() {
    return Result<OutputGeometry>::failure(
        {ErrorCode::validation_error, "An X11 output geometry is invalid.",
         "Use nonnegative nonzero geometry fully contained in the current root rectangle."});
}

[[nodiscard]] Result<OutputTopologySnapshot> invalidTopology() {
    return Result<OutputTopologySnapshot>::failure(
        {ErrorCode::validation_error, "The X11 output topology is invalid.",
         "Discard the complete observation and retain the previous validated topology."});
}

[[nodiscard]] Result<OutputTopologySnapshot> oversizedTopology() {
    return Result<OutputTopologySnapshot>::failure(
        {ErrorCode::too_large, "The X11 output topology exceeds its resource bounds.",
         "Discard the complete observation and retain the previous validated topology."});
}

[[nodiscard]] bool checkedContainedEnd(std::int64_t start, std::uint64_t extent,
                                       std::uint32_t rootExtent,
                                       std::uint64_t &endExclusive) noexcept {
    if (start < 0 || extent == 0U) {
        return false;
    }
    const auto unsignedStart = static_cast<std::uint64_t>(start);
    if (extent > std::numeric_limits<std::uint64_t>::max() - unsignedStart) {
        return false;
    }
    endExclusive = unsignedStart + extent;
    return unsignedStart <= std::numeric_limits<std::uint32_t>::max() &&
           extent <= std::numeric_limits<std::uint32_t>::max() && endExclusive <= rootExtent;
}

[[nodiscard]] bool orderedBefore(const ActiveOutput &left, const ActiveOutput &right) noexcept {
    return std::tuple{left.geometry.yPx(), left.geometry.xPx(), left.outputId.value()} <
           std::tuple{right.geometry.yPx(), right.geometry.xPx(), right.outputId.value()};
}

} // namespace

Result<OutputId> OutputId::fromProtocol(Value value) {
    if (value == 0U) {
        return invalidIdentifier<OutputId>("output");
    }
    return Result<OutputId>::success(OutputId{value});
}

Result<CrtcId> CrtcId::fromProtocol(Value value) {
    if (value == 0U) {
        return invalidIdentifier<CrtcId>("CRTC");
    }
    return Result<CrtcId>::success(CrtcId{value});
}

Result<RootGeometry> RootGeometry::create(std::uint64_t widthPx, std::uint64_t heightPx) {
    if (widthPx == 0U || heightPx == 0U || widthPx > std::numeric_limits<std::uint32_t>::max() ||
        heightPx > std::numeric_limits<std::uint32_t>::max()) {
        return invalidRoot();
    }
    return Result<RootGeometry>::success(
        RootGeometry{static_cast<std::uint32_t>(widthPx), static_cast<std::uint32_t>(heightPx)});
}

Result<RootGeometry> RootGeometry::fromScreenInfo(const ScreenInfo &screen) {
    return create(screen.widthPx, screen.heightPx);
}

Result<OutputGeometry> OutputGeometry::create(RootGeometry root, std::int64_t xPx, std::int64_t yPx,
                                              std::uint64_t widthPx, std::uint64_t heightPx) {
    std::uint64_t right = 0U;
    std::uint64_t bottom = 0U;
    if (!checkedContainedEnd(xPx, widthPx, root.widthPx(), right) ||
        !checkedContainedEnd(yPx, heightPx, root.heightPx(), bottom)) {
        return invalidOutput();
    }
    return Result<OutputGeometry>::success(
        OutputGeometry{static_cast<std::uint32_t>(xPx), static_cast<std::uint32_t>(yPx),
                       static_cast<std::uint32_t>(widthPx), static_cast<std::uint32_t>(heightPx)});
}

OutputTopologySnapshot::OutputTopologySnapshot(bool randrAvailable, RootGeometry coreRoot,
                                               std::vector<ActiveOutput> activeOutputs,
                                               OutputSelection selection) noexcept
    : randr_available_(randrAvailable), core_root_(coreRoot),
      active_outputs_(std::move(activeOutputs)), selection_(std::move(selection)) {}

OutputSelection OutputSelection::coreRootFallback(RootGeometry root) {
    auto geometry = OutputGeometry::create(root, 0, 0, root.widthPx(), root.heightPx());
    return OutputSelection{std::nullopt, std::nullopt, geometry.value(),
                           OutputSelectionReason::coreRootFallback};
}

OutputTopologySnapshot coreRootFallbackTopology(RootGeometry root) {
    return OutputTopologySnapshot{false, root, {}, OutputSelection::coreRootFallback(root)};
}

Result<OutputTopologySnapshot> buildOutputTopology(const OutputTopologyObservation &observation) {
    if (observation.resourceOutputCount > maximumTopologyOutputs ||
        observation.activeOutputs.size() > maximumTopologyOutputs ||
        observation.resourceCrtcCount > maximumTopologyCrtcs ||
        observation.resourceModeCount > maximumTopologyModes) {
        return oversizedTopology();
    }
    if (!observation.randrAvailable) {
        if (observation.resourceOutputCount != 0U || observation.resourceCrtcCount != 0U ||
            observation.resourceModeCount != 0U || !observation.activeOutputs.empty() ||
            observation.primary) {
            return invalidTopology();
        }
        return Result<OutputTopologySnapshot>::success(
            coreRootFallbackTopology(observation.coreRoot));
    }
    if (observation.activeOutputs.size() > observation.resourceOutputCount ||
        (!observation.activeOutputs.empty() &&
         (observation.resourceCrtcCount == 0U || observation.resourceModeCount == 0U))) {
        return invalidTopology();
    }

    std::vector<ActiveOutput> activeOutputs;
    activeOutputs.reserve(observation.activeOutputs.size());
    std::vector<std::pair<CrtcId, OutputGeometry>> crtcGeometries;
    crtcGeometries.reserve(observation.activeOutputs.size());
    for (const auto &candidate : observation.activeOutputs) {
        if (std::find_if(activeOutputs.begin(), activeOutputs.end(),
                         [&candidate](const auto &item) {
                             return item.outputId == candidate.outputId;
                         }) != activeOutputs.end()) {
            return invalidTopology();
        }
        auto geometry = OutputGeometry::create(observation.coreRoot, candidate.xPx, candidate.yPx,
                                               candidate.widthPx, candidate.heightPx);
        if (!geometry) {
            return invalidTopology();
        }
        const auto matchingCrtc =
            std::find_if(crtcGeometries.begin(), crtcGeometries.end(),
                         [&candidate](const auto &item) { return item.first == candidate.crtcId; });
        if (matchingCrtc != crtcGeometries.end() && matchingCrtc->second != geometry.value()) {
            return invalidTopology();
        }
        if (matchingCrtc == crtcGeometries.end()) {
            crtcGeometries.emplace_back(candidate.crtcId, geometry.value());
        }
        activeOutputs.push_back(
            ActiveOutput{candidate.outputId, candidate.crtcId, geometry.value()});
    }
    if (crtcGeometries.size() > observation.resourceCrtcCount) {
        return invalidTopology();
    }

    std::sort(activeOutputs.begin(), activeOutputs.end(), [](const auto &left, const auto &right) {
        return left.outputId.value() < right.outputId.value();
    });

    const ActiveOutput *selected = nullptr;
    OutputSelectionReason reason = OutputSelectionReason::coreRootFallback;
    if (observation.primary) {
        const auto primary = std::find_if(activeOutputs.begin(), activeOutputs.end(),
                                          [&observation](const auto &output) {
                                              return output.outputId == observation.primary.value();
                                          });
        if (primary != activeOutputs.end()) {
            selected = &*primary;
            reason = OutputSelectionReason::randrPrimary;
        }
    }
    if (selected == nullptr) {
        for (const auto &output : activeOutputs) {
            if (output.geometry.xPx() == 0U && output.geometry.yPx() == 0U &&
                (selected == nullptr || output.outputId.value() < selected->outputId.value())) {
                selected = &output;
            }
        }
        if (selected != nullptr) {
            reason = OutputSelectionReason::randrRootOrigin;
        }
    }
    if (selected == nullptr && !activeOutputs.empty()) {
        selected = &*std::min_element(activeOutputs.begin(), activeOutputs.end(), orderedBefore);
        reason = OutputSelectionReason::randrOrdered;
    }

    OutputSelection selection =
        selected == nullptr
            ? OutputSelection::coreRootFallback(observation.coreRoot)
            : OutputSelection{selected->outputId, selected->crtcId, selected->geometry, reason};
    return Result<OutputTopologySnapshot>::success(OutputTopologySnapshot{
        true, observation.coreRoot, std::move(activeOutputs), std::move(selection)});
}

} // namespace prismdrake::x11
