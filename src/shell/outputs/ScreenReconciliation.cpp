#include "ScreenReconciliation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace prismdrake::shell::outputs {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidObservation() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The output screen observation is invalid.",
         "Discard the complete observation and retain the previous placement generation."});
}

[[nodiscard]] bool validLogicalRect(const LogicalRect &rect) noexcept {
    if (rect.width == 0U || rect.height == 0U) {
        return false;
    }
    const auto right = static_cast<std::int64_t>(rect.x) + rect.width;
    const auto bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    return right <= std::numeric_limits<std::int32_t>::max() &&
           bottom <= std::numeric_limits<std::int32_t>::max();
}

[[nodiscard]] bool validScale(double scale) noexcept {
    return std::isfinite(scale) && scale >= minimumDevicePixelRatio &&
           scale <= maximumDevicePixelRatio;
}

[[nodiscard]] bool scaledExtentMatches(std::uint32_t logical, double scale,
                                       std::uint32_t native) noexcept {
    const double scaled = static_cast<double>(logical) * scale;
    if (!std::isfinite(scaled) || scaled > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    return std::llround(scaled) == static_cast<long long>(native);
}

[[nodiscard]] bool relativeGeometryMatches(const ReconciledOutput &left,
                                           const ReconciledOutput &right) noexcept {
    const auto &leftLogical = *left.logicalGeometry;
    const auto &rightLogical = *right.logicalGeometry;
    const auto leftNativeRight =
        static_cast<std::uint64_t>(left.nativeGeometry.xPx()) + left.nativeGeometry.widthPx();
    const auto rightNativeRight =
        static_cast<std::uint64_t>(right.nativeGeometry.xPx()) + right.nativeGeometry.widthPx();
    const auto leftNativeBottom =
        static_cast<std::uint64_t>(left.nativeGeometry.yPx()) + left.nativeGeometry.heightPx();
    const auto rightNativeBottom =
        static_cast<std::uint64_t>(right.nativeGeometry.yPx()) + right.nativeGeometry.heightPx();
    const auto leftLogicalRight = static_cast<std::int64_t>(leftLogical.x) + leftLogical.width;
    const auto rightLogicalRight = static_cast<std::int64_t>(rightLogical.x) + rightLogical.width;
    const auto leftLogicalBottom = static_cast<std::int64_t>(leftLogical.y) + leftLogical.height;
    const auto rightLogicalBottom = static_cast<std::int64_t>(rightLogical.y) + rightLogical.height;

    return !(leftNativeRight <= right.nativeGeometry.xPx() && leftLogicalRight > rightLogical.x) &&
           !(rightNativeRight <= left.nativeGeometry.xPx() && rightLogicalRight > leftLogical.x) &&
           !(leftNativeBottom <= right.nativeGeometry.yPx() &&
             leftLogicalBottom > rightLogical.y) &&
           !(rightNativeBottom <= left.nativeGeometry.yPx() && rightLogicalBottom > leftLogical.y);
}

} // namespace

ReconciledTopologySnapshot::ReconciledTopologySnapshot(
    std::uint64_t generation, x11::RootGeometry root, std::vector<ReconciledOutput> outputs,
    std::optional<std::string> primaryOutputName) noexcept
    : generation_(generation), root_(root), outputs_(std::move(outputs)),
      primary_output_name_(std::move(primaryOutputName)) {}

Result<ReconciledTopologySnapshot>
reconcileScreens(std::uint64_t generation, const x11::OutputTopologySnapshot &topology,
                 std::span<const QtScreenObservation> qtScreens) {
    if (generation == 0U || qtScreens.size() > x11::maximumTopologyOutputs) {
        return invalidObservation<ReconciledTopologySnapshot>();
    }

    for (std::size_t index = 0U; index < qtScreens.size(); ++index) {
        const auto &screen = qtScreens[index];
        if (!x11::isStableOutputName(screen.name) || !validLogicalRect(screen.logicalGeometry) ||
            !validScale(screen.devicePixelRatio) ||
            std::find_if(qtScreens.begin(), qtScreens.begin() + static_cast<std::ptrdiff_t>(index),
                         [&screen](const auto &prior) { return prior.name == screen.name; }) !=
                qtScreens.begin() + static_cast<std::ptrdiff_t>(index)) {
            return invalidObservation<ReconciledTopologySnapshot>();
        }
    }

    std::vector<ReconciledOutput> outputs;
    outputs.reserve(topology.activeOutputs().size());
    for (const auto &native : topology.activeOutputs()) {
        if (!x11::isStableOutputName(native.name)) {
            return invalidObservation<ReconciledTopologySnapshot>();
        }
        const auto screen =
            std::find_if(qtScreens.begin(), qtScreens.end(),
                         [&native](const auto &q) { return q.name == native.name; });
        if (screen == qtScreens.end()) {
            outputs.push_back({native.outputId, native.crtcId, native.name, native.geometry,
                               std::nullopt, 1.0, ReconciliationStatus::missingQtScreen, false});
            continue;
        }
        const bool dimensionsMatch =
            scaledExtentMatches(screen->logicalGeometry.width, screen->devicePixelRatio,
                                native.geometry.widthPx()) &&
            scaledExtentMatches(screen->logicalGeometry.height, screen->devicePixelRatio,
                                native.geometry.heightPx());
        outputs.push_back({native.outputId, native.crtcId, native.name, native.geometry,
                           dimensionsMatch ? std::optional{screen->logicalGeometry} : std::nullopt,
                           screen->devicePixelRatio,
                           dimensionsMatch ? ReconciliationStatus::matched
                                           : ReconciliationStatus::geometryMismatch,
                           false});
    }

    std::vector<bool> inconsistent(outputs.size(), false);
    for (std::size_t left = 0U; left < outputs.size(); ++left) {
        if (!outputs[left].available()) {
            continue;
        }
        for (std::size_t right = left + 1U; right < outputs.size(); ++right) {
            if (outputs[right].available() &&
                !relativeGeometryMatches(outputs[left], outputs[right])) {
                inconsistent[left] = true;
                inconsistent[right] = true;
            }
        }
    }
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        if (inconsistent[index]) {
            outputs[index].logicalGeometry.reset();
            outputs[index].status = ReconciliationStatus::geometryMismatch;
        }
    }

    std::optional<std::string> primaryName;
    if (topology.selection().outputId()) {
        const auto primary =
            std::find_if(outputs.begin(), outputs.end(), [&topology](const auto &item) {
                return item.outputId == *topology.selection().outputId() && item.available();
            });
        if (primary != outputs.end()) {
            primary->primary = true;
            primaryName = primary->name;
        }
    }
    if (!primaryName) {
        const auto fallback = std::find_if(outputs.begin(), outputs.end(),
                                           [](const auto &item) { return item.available(); });
        if (fallback != outputs.end()) {
            fallback->primary = true;
            primaryName = fallback->name;
        }
    }

    return Result<ReconciledTopologySnapshot>::success(ReconciledTopologySnapshot{
        generation, topology.coreRoot(), std::move(outputs), std::move(primaryName)});
}

} // namespace prismdrake::shell::outputs
