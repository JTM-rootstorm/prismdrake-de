#pragma once

#include "OutputSurfaceRegistry.hpp"
#include "Result.hpp"
#include "WallpaperLayout.hpp"
#include "WallpaperModel.hpp"

#include <QRect>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::desktop {

struct OutputScaleObservation final {
    std::string outputName;
    double devicePixelRatio;
};

struct DesktopSurfaceState final {
    std::string outputName;
    QRect logicalGeometry;
    double devicePixelRatio;
    std::optional<prismdrake::desktop::WallpaperLayout> wallpaperLayout;
};

/// One complete immutable desktop publication. Every output shares the exact
/// same wallpaper object and settings generation.
class DesktopSurfaceSnapshot final {
  public:
    [[nodiscard]] std::uint64_t topologyGeneration() const noexcept { return topology_generation_; }
    [[nodiscard]] const std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> &
    wallpaper() const noexcept {
        return wallpaper_;
    }
    [[nodiscard]] std::span<const DesktopSurfaceState> surfaces() const noexcept {
        return surfaces_;
    }

  private:
    friend class DesktopSurfaceController;

    DesktopSurfaceSnapshot(std::uint64_t topologyGeneration,
                           std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper,
                           std::vector<DesktopSurfaceState> surfaces) noexcept
        : topology_generation_(topologyGeneration), wallpaper_(std::move(wallpaper)),
          surfaces_(std::move(surfaces)) {}

    std::uint64_t topology_generation_;
    std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper_;
    std::vector<DesktopSurfaceState> surfaces_;
};

class DesktopSurfaceUpdate final {
  public:
    [[nodiscard]] const std::shared_ptr<const DesktopSurfaceSnapshot> &snapshot() const noexcept {
        return snapshot_;
    }

  private:
    friend class DesktopSurfaceController;
    explicit DesktopSurfaceUpdate(std::shared_ptr<const DesktopSurfaceSnapshot> snapshot) noexcept
        : snapshot_(std::move(snapshot)) {}

    std::shared_ptr<const DesktopSurfaceSnapshot> snapshot_;
};

/// Display-free atomic boundary between complete registry placements and the
/// Qt/X11 host. Invalid updates retain the last committed publication.
class DesktopSurfaceController final {
  public:
    [[nodiscard]] foundation::Result<DesktopSurfaceUpdate>
    prepare(const outputs::SurfaceRegistrySnapshot &registry,
            std::span<const OutputScaleObservation> scales,
            std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot> wallpaper) const;

    /// Rebuilds topology from the last safe wallpaper generation. This remains
    /// available while settingsd has no owner.
    [[nodiscard]] foundation::Result<DesktopSurfaceUpdate>
    prepare(const outputs::SurfaceRegistrySnapshot &registry,
            std::span<const OutputScaleObservation> scales) const;

    void commit(DesktopSurfaceUpdate update) noexcept;

    /// Settings-owner loss deliberately does not invalidate current wallpaper.
    void settingsOwnerLost() noexcept { settings_owner_available_ = false; }

    [[nodiscard]] const std::shared_ptr<const DesktopSurfaceSnapshot> &current() const noexcept {
        return current_;
    }
    [[nodiscard]] bool settingsOwnerAvailable() const noexcept { return settings_owner_available_; }

  private:
    std::shared_ptr<const DesktopSurfaceSnapshot> current_;
    bool settings_owner_available_{false};
};

} // namespace prismdrake::shell::desktop
