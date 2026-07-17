#pragma once

#include "OutputTopology.hpp"
#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace prismdrake::x11 {

class X11Connection;
class RootEventStream;

inline constexpr std::uint32_t requiredRandrMajorVersion = 1U;
inline constexpr std::uint32_t requiredRandrMinorVersion = 2U;
inline constexpr std::uint32_t primaryOutputRandrMinorVersion = 3U;
inline constexpr std::uint32_t resourceChangeRandrMinorVersion = 4U;
inline constexpr std::size_t maximumRandrOutputNameBytes = 255U;

inline constexpr std::uint16_t randrScreenChangeMask = 1U;
inline constexpr std::uint16_t randrCrtcChangeMask = 2U;
inline constexpr std::uint16_t randrOutputChangeMask = 4U;
inline constexpr std::uint16_t randrResourceChangeMask = 64U;
inline constexpr std::uint16_t randr12TopologyEventMask =
    randrScreenChangeMask | randrCrtcChangeMask | randrOutputChangeMask;
inline constexpr std::uint16_t randrTopologyEventMask =
    randr12TopologyEventMask | randrResourceChangeMask;

enum class RandrTopologyStatus : std::uint8_t {
    unavailable,
    malformed,
    randr_1_2,
    randr_1_3,
    randr_1_4,
};

[[nodiscard]] std::string_view randrTopologyStatusId(RandrTopologyStatus status) noexcept;

/// One complete bounded wire snapshot. Policy validation and output selection
/// remain in OutputTopology; malformed or unavailable RandR states carry only
/// the fresh core-root fallback.
struct RandrTopologySnapshot final {
    RandrTopologyStatus status;
    ScreenInfo coreScreen;
    std::size_t resourceOutputCount;
    std::size_t resourceCrtcCount;
    std::size_t resourceModeCount;
    std::vector<OutputCandidate> activeOutputs;
    std::optional<OutputId> primary;

    [[nodiscard]] bool randrAvailable() const noexcept {
        return status == RandrTopologyStatus::randr_1_2 ||
               status == RandrTopologyStatus::randr_1_3 || status == RandrTopologyStatus::randr_1_4;
    }
};

/// Minimal copied extension-event header. Inline RandR geometry is deliberately
/// absent: every relevant event requires a complete checked topology query.
struct RandrEventFields final {
    std::uint8_t responseType;
    std::uint8_t notifySubCode;
};

/// Copied identifier lists from one GetOutputInfo reply. No output name bytes
/// are retained or exposed by the topology protocol.
struct RandrOutputInfoListsView final {
    std::uint32_t activeCrtc;
    std::span<std::uint32_t> allowedCrtcs;
    std::span<std::uint32_t> supportedModes;
    std::span<std::uint32_t> clones;
};

/// Sorted unique resource identifiers against which a decoded per-output or
/// per-CRTC reply is checked before any candidate is produced.
struct RandrResourceIdsView final {
    std::span<const std::uint32_t> crtcs;
    std::span<const std::uint32_t> outputs;
    std::span<const std::uint32_t> modes;
};

struct RandrCrtcInfoListsView final {
    std::uint32_t requestedOutput;
    std::span<std::uint32_t> currentOutputs;
    std::span<std::uint32_t> possibleOutputs;
};

/// Pure decoded-list checks used by the bounded wire adapter and negative tests.
/// The copied per-reply lists are sorted in place; no wire-backed memory is used.
[[nodiscard]] bool randrOutputInfoListsAreValid(RandrOutputInfoListsView output,
                                                RandrResourceIdsView resources) noexcept;
[[nodiscard]] bool
randrCrtcInfoListsAreValid(RandrCrtcInfoListsView crtc,
                           std::span<const std::uint32_t> resourceOutputs) noexcept;
[[nodiscard]] bool randrModeNameLengthsMatch(std::span<const std::uint16_t> modeNameLengths,
                                             std::uint16_t namesLength) noexcept;

/// Protocol-neutral classification used by the connection event dispatcher.
[[nodiscard]] bool randrEventRequiresFullRequery(std::uint8_t firstEvent, RandrEventFields event,
                                                 bool supportsResourceChange) noexcept;

/// Connection-proven runtime RandR negotiation and bounded topology operations.
class RandrTopologyProtocol final {
  public:
    [[nodiscard]] static foundation::Result<RandrTopologyProtocol>
    negotiate(X11Connection &connection);

    [[nodiscard]] RandrTopologyStatus status() const noexcept { return status_; }
    [[nodiscard]] std::uint32_t majorVersion() const noexcept { return major_version_; }
    [[nodiscard]] std::uint32_t minorVersion() const noexcept { return minor_version_; }

    [[nodiscard]] foundation::Result<RandrTopologySnapshot> query(X11Connection &connection) const;

    /// True means discard inline event data and perform one complete query().
    [[nodiscard]] bool eventRequiresFullRequery(RandrEventFields event) const noexcept;

  private:
    friend class RootEventStream;

    RandrTopologyProtocol(RandrTopologyStatus status, std::uint32_t majorVersion,
                          std::uint32_t minorVersion, std::uint8_t firstEvent,
                          std::uint64_t connectionIdentity) noexcept
        : status_(status), major_version_(majorVersion), minor_version_(minorVersion),
          first_event_(firstEvent), connection_identity_(connectionIdentity) {}

    /// Selects exactly the topology-refresh masks owned by RootEventStream.
    /// Returns false without issuing a request when RandR 1.2 is unavailable.
    [[nodiscard]] foundation::Result<bool> selectTopologyEvents(X11Connection &connection) const;

    [[nodiscard]] bool belongsTo(std::uint64_t connectionIdentity) const noexcept {
        return connectionIdentity != 0U && connectionIdentity == connection_identity_;
    }

    RandrTopologyStatus status_;
    std::uint32_t major_version_;
    std::uint32_t minor_version_;
    std::uint8_t first_event_;
    std::uint64_t connection_identity_;
};

} // namespace prismdrake::x11
