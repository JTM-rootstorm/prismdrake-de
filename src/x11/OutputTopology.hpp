#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace prismdrake::x11 {

inline constexpr std::size_t maximumTopologyOutputs = 64U;
inline constexpr std::size_t maximumTopologyCrtcs = 64U;
inline constexpr std::size_t maximumTopologyModes = 4096U;
/// Aggregate ceiling for each future wire-level RandR reply consumed by this policy layer.
inline constexpr std::size_t maximumTopologyReplyBytes = 1024U * 1024U;
inline constexpr double pd1OutputScale = 1.0;

/// Explicit non-owning RandR output identifier. Zero denotes None and is invalid.
class OutputId final {
  public:
    using Value = std::uint32_t;

    [[nodiscard]] static foundation::Result<OutputId> fromProtocol(Value value);
    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const OutputId &, const OutputId &) = default;

  private:
    explicit OutputId(Value value) noexcept : value_(value) {}

    Value value_;
};

/// Explicit non-owning RandR CRTC identifier. Zero denotes None and is invalid.
class CrtcId final {
  public:
    using Value = std::uint32_t;

    [[nodiscard]] static foundation::Result<CrtcId> fromProtocol(Value value);
    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const CrtcId &, const CrtcId &) = default;

  private:
    explicit CrtcId(Value value) noexcept : value_(value) {}

    Value value_;
};

/// Validated nonzero root rectangle dimensions in root coordinates.
class RootGeometry final {
  public:
    [[nodiscard]] static foundation::Result<RootGeometry> create(std::uint64_t widthPx,
                                                                 std::uint64_t heightPx);
    [[nodiscard]] static foundation::Result<RootGeometry> fromScreenInfo(const ScreenInfo &screen);

    [[nodiscard]] std::uint32_t widthPx() const noexcept { return width_px_; }
    [[nodiscard]] std::uint32_t heightPx() const noexcept { return height_px_; }

    friend bool operator==(const RootGeometry &, const RootGeometry &) = default;

  private:
    RootGeometry(std::uint32_t widthPx, std::uint32_t heightPx) noexcept
        : width_px_(widthPx), height_px_(heightPx) {}

    std::uint32_t width_px_;
    std::uint32_t height_px_;
};

/// Validated nonempty output rectangle fully contained in its root rectangle.
class OutputGeometry final {
  public:
    [[nodiscard]] static foundation::Result<OutputGeometry>
    create(RootGeometry root, std::int64_t xPx, std::int64_t yPx, std::uint64_t widthPx,
           std::uint64_t heightPx);

    [[nodiscard]] std::uint32_t xPx() const noexcept { return x_px_; }
    [[nodiscard]] std::uint32_t yPx() const noexcept { return y_px_; }
    [[nodiscard]] std::uint32_t widthPx() const noexcept { return width_px_; }
    [[nodiscard]] std::uint32_t heightPx() const noexcept { return height_px_; }
    [[nodiscard]] constexpr double scale() const noexcept { return pd1OutputScale; }

    friend bool operator==(const OutputGeometry &, const OutputGeometry &) = default;

  private:
    OutputGeometry(std::uint32_t xPx, std::uint32_t yPx, std::uint32_t widthPx,
                   std::uint32_t heightPx) noexcept
        : x_px_(xPx), y_px_(yPx), width_px_(widthPx), height_px_(heightPx) {}

    std::uint32_t x_px_;
    std::uint32_t y_px_;
    std::uint32_t width_px_;
    std::uint32_t height_px_;
};

/// One decoded active CONNECTED RandR output with a CRTC, mode, and nonzero geometry.
/// Coordinates and dimensions remain wide until the complete topology is validated.
struct OutputCandidate final {
    OutputId outputId;
    CrtcId crtcId;
    std::int64_t xPx;
    std::int64_t yPx;
    std::uint64_t widthPx;
    std::uint64_t heightPx;
};

/// Complete decoded input to the display-free topology validator and selector.
struct OutputTopologyObservation final {
    bool randrAvailable;
    RootGeometry coreRoot;
    std::size_t resourceOutputCount;
    std::size_t resourceCrtcCount;
    std::size_t resourceModeCount;
    std::vector<OutputCandidate> activeOutputs;
    std::optional<OutputId> primary;
};

struct ActiveOutput final {
    OutputId outputId;
    CrtcId crtcId;
    OutputGeometry geometry;

    friend bool operator==(const ActiveOutput &, const ActiveOutput &) = default;
};

enum class OutputSelectionReason : std::uint8_t {
    randrPrimary,
    randrRootOrigin,
    randrOrdered,
    coreRootFallback,
};

/// Deterministically selected PD1 panel output, or the core-root fallback without RandR IDs.
class OutputSelection final {
  public:
    [[nodiscard]] static OutputSelection coreRootFallback(RootGeometry root);

    [[nodiscard]] const std::optional<OutputId> &outputId() const noexcept { return output_id_; }
    [[nodiscard]] const std::optional<CrtcId> &crtcId() const noexcept { return crtc_id_; }
    [[nodiscard]] const OutputGeometry &geometry() const noexcept { return geometry_; }
    [[nodiscard]] OutputSelectionReason reason() const noexcept { return reason_; }
    [[nodiscard]] constexpr double scale() const noexcept { return pd1OutputScale; }

  private:
    friend class OutputTopologySnapshot;
    friend foundation::Result<class OutputTopologySnapshot>
    buildOutputTopology(const OutputTopologyObservation &observation);

    OutputSelection(std::optional<OutputId> outputId, std::optional<CrtcId> crtcId,
                    OutputGeometry geometry, OutputSelectionReason reason) noexcept
        : output_id_(outputId), crtc_id_(crtcId), geometry_(geometry), reason_(reason) {}

    std::optional<OutputId> output_id_;
    std::optional<CrtcId> crtc_id_;
    OutputGeometry geometry_;
    OutputSelectionReason reason_;
};

/// Immutable all-or-nothing validated topology and its deterministic PD1 selection.
class OutputTopologySnapshot final {
  public:
    [[nodiscard]] bool randrAvailable() const noexcept { return randr_available_; }
    [[nodiscard]] const RootGeometry &coreRoot() const noexcept { return core_root_; }
    [[nodiscard]] std::span<const ActiveOutput> activeOutputs() const noexcept {
        return active_outputs_;
    }
    [[nodiscard]] const OutputSelection &selection() const noexcept { return selection_; }

  private:
    friend foundation::Result<OutputTopologySnapshot>
    buildOutputTopology(const OutputTopologyObservation &observation);
    friend OutputTopologySnapshot coreRootFallbackTopology(RootGeometry root);

    OutputTopologySnapshot(bool randrAvailable, RootGeometry coreRoot,
                           std::vector<ActiveOutput> activeOutputs,
                           OutputSelection selection) noexcept;

    bool randr_available_;
    RootGeometry core_root_;
    std::vector<ActiveOutput> active_outputs_;
    OutputSelection selection_;
};

/// Validates the complete observation before publishing any snapshot and applies the PD1 policy.
[[nodiscard]] foundation::Result<OutputTopologySnapshot>
buildOutputTopology(const OutputTopologyObservation &observation);

/// Produces the explicit startup fallback when no previous valid snapshot can be retained.
[[nodiscard]] OutputTopologySnapshot coreRootFallbackTopology(RootGeometry root);

} // namespace prismdrake::x11
