#pragma once

#include "OutputSurfaceRegistry.hpp"
#include "OutputTopology.hpp"
#include "ScreenReconciliation.hpp"
#include "WallpaperModel.hpp"

#include <QColor>
#include <QImage>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::desktop::test {

[[nodiscard]] inline x11::OutputId outputId(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] inline x11::CrtcId crtcId(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] inline x11::OutputCandidate candidate(std::uint32_t output, std::uint32_t crtc,
                                                    std::string name, std::int64_t x,
                                                    std::uint64_t width, std::uint64_t height) {
    return {outputId(output), crtcId(crtc), x, 0, width, height, std::move(name)};
}

class RegistryHarness final {
  public:
    RegistryHarness() : registry_(outputs::OutputSurfaceRegistry::create(48U).value()) {}

    [[nodiscard]] outputs::SurfaceRegistrySnapshot
    observe(std::uint64_t generation, bool includeRight = true,
            outputs::LogicalRect leftGeometry = {0, 0, 512U, 768U}) {
        const std::uint64_t rootWidth = includeRight ? 1024U : 512U;
        std::vector<x11::OutputCandidate> candidates{
            candidate(1U, 10U, "LEFT-1", 0, leftGeometry.width, leftGeometry.height)};
        std::vector<outputs::QtScreenObservation> screens{
            {"LEFT-1", leftGeometry, 1.0},
        };
        if (includeRight) {
            candidates.push_back(candidate(2U, 20U, "RIGHT-1", 512, 512U, 768U));
            screens.push_back({"RIGHT-1", {512, 0, 512U, 768U}, 1.0});
        }
        const auto root = x11::RootGeometry::create(rootWidth, 768U).value();
        auto topology = x11::buildOutputTopology({true, root, candidates.size(), candidates.size(),
                                                  1U, std::move(candidates), outputId(1U)})
                            .value();
        auto reconciled = outputs::reconcileScreens(generation, topology, screens).value();
        return registry_.observe(reconciled, {outputs::PanelOutputPolicy::primary, {}}).value();
    }

    [[nodiscard]] outputs::SurfaceRegistrySnapshot observeMixedScale(std::uint64_t generation) {
        const auto root = x11::RootGeometry::create(4480U, 1440U).value();
        std::vector<x11::OutputCandidate> candidates{
            candidate(1U, 10U, "HDMI-1", 2560, 1920U, 1080U),
            candidate(2U, 20U, "DP-1", 0, 2560U, 1440U),
        };
        auto topology = x11::buildOutputTopology({true, root, candidates.size(), candidates.size(),
                                                  2U, std::move(candidates), outputId(1U)})
                            .value();
        const std::array screens{
            outputs::QtScreenObservation{"HDMI-1", {0, 0, 1920U, 1080U}, 1.0},
            outputs::QtScreenObservation{"DP-1", {-1280, 0, 1280U, 720U}, 2.0},
        };
        auto reconciled = outputs::reconcileScreens(generation, topology, screens).value();
        return registry_.observe(reconciled, {outputs::PanelOutputPolicy::primary, {}}).value();
    }

  private:
    outputs::OutputSurfaceRegistry registry_;
};

[[nodiscard]] inline std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot>
solidWallpaper(std::uint64_t generation, QColor color = QColor{"#182033"}) {
    return std::make_shared<const prismdrake::desktop::WallpaperSnapshot>(
        prismdrake::desktop::WallpaperSnapshot{generation,
                                               std::move(color),
                                               config::WallpaperMode::solid,
                                               prismdrake::desktop::WallpaperLoadStatus::solid,
                                               {},
                                               {}});
}

[[nodiscard]] inline std::shared_ptr<const prismdrake::desktop::WallpaperSnapshot>
imageWallpaper(std::uint64_t generation, config::WallpaperMode mode = config::WallpaperMode::fill) {
    auto image = std::make_shared<const QImage>(QImage{64, 32, QImage::Format_RGBA8888});
    return std::make_shared<const prismdrake::desktop::WallpaperSnapshot>(
        prismdrake::desktop::WallpaperSnapshot{generation, QColor{"#182033"}, mode,
                                               prismdrake::desktop::WallpaperLoadStatus::ready,
                                               image->size(), std::move(image)});
}

} // namespace prismdrake::shell::desktop::test
