#include "DesktopSurfaceController.hpp"

#include "OutputTopology.hpp"
#include "ScreenReconciliation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace prismdrake::shell::desktop {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidPublication() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "The desktop surface publication is invalid.",
         "Use one complete newer registry generation, one scale per desktop, and one immutable "
         "wallpaper snapshot."});
}

[[nodiscard]] const OutputScaleObservation *
findScale(std::span<const OutputScaleObservation> scales, std::string_view outputName) {
    const auto found = std::find_if(scales.begin(), scales.end(), [outputName](const auto &scale) {
        return scale.outputName == outputName;
    });
    return found == scales.end() ? nullptr : &*found;
}

[[nodiscard]] bool representableGeometry(const outputs::LogicalRect &geometry) noexcept {
    if (geometry.width == 0U || geometry.height == 0U ||
        geometry.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        geometry.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto right = static_cast<std::int64_t>(geometry.x) + geometry.width - 1U;
    const auto bottom = static_cast<std::int64_t>(geometry.y) + geometry.height - 1U;
    return right <= std::numeric_limits<int>::max() && bottom <= std::numeric_limits<int>::max();
}

} // namespace

Result<DesktopSurfaceUpdate> DesktopSurfaceController::prepare(
    const outputs::SurfaceRegistrySnapshot &registry,
    std::span<const OutputScaleObservation> scales,
    std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper) const {
    if (registry.topologyGeneration() == 0U || !wallpaper || wallpaper->generation == 0U ||
        (current_ && registry.topologyGeneration() < current_->topologyGeneration()) ||
        (current_ && settings_owner_available_ &&
         wallpaper->generation < current_->wallpaper()->generation) ||
        (current_ && settings_owner_available_ &&
         wallpaper->generation == current_->wallpaper()->generation &&
         wallpaper != current_->wallpaper()) ||
        scales.size() != registry.desktops().size()) {
        return invalidPublication<DesktopSurfaceUpdate>();
    }

    std::set<std::string> outputNames;
    std::set<std::string> scaleNames;
    for (const auto &scale : scales) {
        if (!x11::isStableOutputName(scale.outputName) ||
            !scaleNames.insert(scale.outputName).second || !std::isfinite(scale.devicePixelRatio) ||
            scale.devicePixelRatio < outputs::minimumDevicePixelRatio ||
            scale.devicePixelRatio > outputs::maximumDevicePixelRatio) {
            return invalidPublication<DesktopSurfaceUpdate>();
        }
    }

    std::vector<DesktopSurfaceState> surfaces;
    surfaces.reserve(registry.desktops().size());
    for (const auto &placement : registry.desktops()) {
        if (placement.kind != outputs::SurfaceKind::desktop ||
            placement.topologyGeneration != registry.topologyGeneration() ||
            placement.panelStrut.has_value() || !x11::isStableOutputName(placement.outputName) ||
            !outputNames.insert(placement.outputName).second ||
            !representableGeometry(placement.logicalGeometry)) {
            return invalidPublication<DesktopSurfaceUpdate>();
        }
        const auto *scale = findScale(scales, placement.outputName);
        if (scale == nullptr) {
            return invalidPublication<DesktopSurfaceUpdate>();
        }
        const auto &geometry = placement.logicalGeometry;
        std::optional<prismdrake::desktop::WallpaperLayout> layout;
        if (wallpaper->hasImage()) {
            auto calculated = prismdrake::desktop::calculateWallpaperLayout(
                wallpaper->mode, wallpaper->sourceSize,
                QSizeF{static_cast<double>(geometry.width), static_cast<double>(geometry.height)},
                scale->devicePixelRatio);
            if (!calculated) {
                return invalidPublication<DesktopSurfaceUpdate>();
            }
            layout.emplace(*calculated);
        }
        surfaces.push_back({placement.outputName,
                            QRect{geometry.x, geometry.y, static_cast<int>(geometry.width),
                                  static_cast<int>(geometry.height)},
                            scale->devicePixelRatio, std::move(layout)});
    }

    auto snapshot = std::shared_ptr<const DesktopSurfaceSnapshot>{new DesktopSurfaceSnapshot(
        registry.topologyGeneration(), std::move(wallpaper), std::move(surfaces))};
    return Result<DesktopSurfaceUpdate>::success(DesktopSurfaceUpdate{std::move(snapshot)});
}

Result<DesktopSurfaceUpdate>
DesktopSurfaceController::prepare(const outputs::SurfaceRegistrySnapshot &registry,
                                  std::span<const OutputScaleObservation> scales) const {
    if (!current_) {
        return invalidPublication<DesktopSurfaceUpdate>();
    }
    return prepare(registry, scales, current_->wallpaper());
}

void DesktopSurfaceController::commit(DesktopSurfaceUpdate update) noexcept {
    if (!update.snapshot_) {
        return;
    }
    const bool wallpaperChanged =
        !current_ || update.snapshot_->wallpaper() != current_->wallpaper();
    current_ = std::move(update.snapshot_);
    if (wallpaperChanged) {
        settings_owner_available_ = true;
    }
}

} // namespace prismdrake::shell::desktop
