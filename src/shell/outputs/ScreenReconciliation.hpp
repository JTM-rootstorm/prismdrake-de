#pragma once

#include "OutputTopology.hpp"
#include "Result.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace prismdrake::shell::outputs {

inline constexpr double minimumDevicePixelRatio = 0.25;
inline constexpr double maximumDevicePixelRatio = 8.0;

/// Checked Qt logical coordinates copied into a toolkit-neutral observation.
struct LogicalRect final {
    std::int32_t x;
    std::int32_t y;
    std::uint32_t width;
    std::uint32_t height;

    friend bool operator==(const LogicalRect &, const LogicalRect &) = default;
};

/// Value-only screen observation. Qt object lifetimes never cross this boundary.
struct QtScreenObservation final {
    std::string name;
    LogicalRect logicalGeometry;
    double devicePixelRatio;
};

enum class ReconciliationStatus : std::uint8_t {
    matched,
    missingQtScreen,
    geometryMismatch,
};

struct ReconciledOutput final {
    x11::OutputId outputId;
    x11::CrtcId crtcId;
    std::string name;
    x11::OutputGeometry nativeGeometry;
    std::optional<LogicalRect> logicalGeometry;
    double devicePixelRatio;
    ReconciliationStatus status;
    bool primary;

    [[nodiscard]] bool available() const noexcept {
        return status == ReconciliationStatus::matched && logicalGeometry.has_value();
    }
};

/// One immutable all-output observation. Unmatched RandR outputs remain
/// explicit but are unavailable for shell surface placement.
class ReconciledTopologySnapshot final {
  public:
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const x11::RootGeometry &root() const noexcept { return root_; }
    [[nodiscard]] std::span<const ReconciledOutput> outputs() const noexcept { return outputs_; }
    [[nodiscard]] const std::optional<std::string> &primaryOutputName() const noexcept {
        return primary_output_name_;
    }

  private:
    friend foundation::Result<ReconciledTopologySnapshot>
    reconcileScreens(std::uint64_t, const x11::OutputTopologySnapshot &,
                     std::span<const QtScreenObservation>);

    ReconciledTopologySnapshot(std::uint64_t generation, x11::RootGeometry root,
                               std::vector<ReconciledOutput> outputs,
                               std::optional<std::string> primaryOutputName) noexcept;

    std::uint64_t generation_;
    x11::RootGeometry root_;
    std::vector<ReconciledOutput> outputs_;
    std::optional<std::string> primary_output_name_;
};

/// Reconciles bounded Qt screen values with the validated RandR topology by
/// stable name and scaled dimensions. Physical dimensions are never consumed.
[[nodiscard]] foundation::Result<ReconciledTopologySnapshot>
reconcileScreens(std::uint64_t generation, const x11::OutputTopologySnapshot &topology,
                 std::span<const QtScreenObservation> qtScreens);

} // namespace prismdrake::shell::outputs
