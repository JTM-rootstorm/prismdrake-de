#pragma once

#include "AtomCache.hpp"
#include "Result.hpp"
#include "X11Connection.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::x11 {

class RandrTopologyProtocol;

inline constexpr std::uint8_t syntheticEventBit = 0x80U;
inline constexpr std::uint8_t coreEventTypeMask = 0x7fU;
inline constexpr std::uint8_t createNotifyEventType = 16U;
inline constexpr std::uint8_t destroyNotifyEventType = 17U;
inline constexpr std::uint8_t configureNotifyEventType = 22U;
inline constexpr std::uint8_t propertyNotifyEventType = 28U;

inline constexpr std::uint32_t structureNotifyEventMask = 0x00020000U;
inline constexpr std::uint32_t substructureNotifyEventMask = 0x00080000U;
inline constexpr std::uint32_t substructureRedirectEventMask = 0x00100000U;
inline constexpr std::uint32_t propertyChangeEventMask = 0x00400000U;
inline constexpr std::uint32_t rootEventSelectionMask =
    structureNotifyEventMask | substructureNotifyEventMask | propertyChangeEventMask;
static_assert((rootEventSelectionMask & substructureRedirectEventMask) == 0U);

inline constexpr std::size_t maximumRootEventsPerDrain = 256U;

/// Protocol-neutral fields copied from one core CreateNotify event.
struct CreateNotifyFields final {
    std::uint8_t responseType;
    WindowId::Value parent;
    WindowId::Value window;
    std::int16_t x;
    std::int16_t y;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t borderWidth;
    bool overrideRedirect;
};

/// Protocol-neutral fields copied from one core DestroyNotify event.
struct DestroyNotifyFields final {
    std::uint8_t responseType;
    WindowId::Value eventWindow;
    WindowId::Value window;
};

/// Protocol-neutral fields copied from one core ConfigureNotify event.
struct ConfigureNotifyFields final {
    std::uint8_t responseType;
    WindowId::Value eventWindow;
    WindowId::Value window;
    std::int16_t x;
    std::int16_t y;
    std::uint16_t width;
    std::uint16_t height;
    std::uint16_t borderWidth;
};

/// Protocol-neutral fields copied from one core PropertyNotify event.
struct PropertyNotifyFields final {
    std::uint8_t responseType;
    WindowId::Value window;
    std::optional<AtomId> atom;
    std::uint8_t state;
};

/// Redacted protocol-error fields copied without the stale resource identifier.
struct ProtocolErrorFields final {
    std::uint8_t responseType;
    std::uint8_t errorCode;
    std::uint8_t majorCode;
    std::uint16_t minorCode;
};

using CoreRootEventFields =
    std::variant<CreateNotifyFields, DestroyNotifyFields, ConfigureNotifyFields,
                 PropertyNotifyFields, ProtocolErrorFields>;

enum class ClientTopologyChange : std::uint8_t {
    created,
    destroyed,
};

/// Non-authoritative hint that the WM-owned client snapshot may need refreshing.
struct ClientTopologyHint final {
    ClientTopologyChange change;
    bool synthetic;

    friend bool operator==(const ClientTopologyHint &, const ClientTopologyHint &) = default;
};

enum class RootPropertyState : std::uint8_t {
    newValue,
    deleted,
};

/// Non-authoritative hint that one root property should be read again.
struct RootPropertyHint final {
    AtomId atom;
    RootPropertyState state;
    bool synthetic;

    friend bool operator==(const RootPropertyHint &, const RootPropertyHint &) = default;
};

/// Non-authoritative hint that core root geometry should be queried again.
struct RootGeometryHint final {
    bool synthetic;

    friend bool operator==(const RootGeometryHint &, const RootGeometryHint &) = default;
};

/// Non-authoritative hint that the negotiated output topology should be
/// queried again. Inline extension-event geometry is never retained.
struct OutputTopologyRefreshHint final {
    bool synthetic;

    friend bool operator==(const OutputTopologyRefreshHint &,
                           const OutputTopologyRefreshHint &) = default;
};

/// Recoverable hint that a checked state refresh should be attempted after one
/// asynchronous X11 protocol error. Error resource identifiers are never retained.
struct ProtocolErrorHint final {
    friend bool operator==(const ProtocolErrorHint &, const ProtocolErrorHint &) = default;
};

using RootEvent = std::variant<ClientTopologyHint, RootPropertyHint, RootGeometryHint,
                               OutputTopologyRefreshHint, ProtocolErrorHint>;

/// Validates one copied core event. Unrelated root traffic returns an empty
/// optional; malformed relevant traffic returns a bounded redacted error.
[[nodiscard]] foundation::Result<std::optional<RootEvent>>
decodeRootEvent(const CoreRootEventFields &fields, WindowId root);

struct RootEventBatch final {
    std::vector<RootEvent> events;
    /// True when the raw-event examination budget was consumed. The caller
    /// should schedule another event-loop turn without busy polling.
    bool examinationLimitReached;
};

/// One event-driven core-XCB root subscription on an owned X11 connection,
/// optionally combined with negotiated RandR topology events.
///
/// Events are refresh hints only. This class never owns or mutates authoritative
/// window-manager state and never selects SubstructureRedirect.
class RootEventStream final {
  public:
    /// Creates a core-only event stream without selecting extension events.
    [[nodiscard]] static foundation::Result<RootEventStream> create(X11Connection &connection);

    /// Atomically owns the sole queue consumer and the negotiated RandR
    /// subscription. The protocol must have been negotiated on connection.
    [[nodiscard]] static foundation::Result<RootEventStream>
    create(X11Connection &connection, const RandrTopologyProtocol &randr);

    RootEventStream(const RootEventStream &) = delete;
    RootEventStream &operator=(const RootEventStream &) = delete;
    RootEventStream(RootEventStream &&other) noexcept;
    RootEventStream &operator=(RootEventStream &&) = delete;
    ~RootEventStream();

    /// Drains at most examinationLimit raw events without blocking.
    [[nodiscard]] foundation::Result<RootEventBatch>
    drain(std::size_t examinationLimit = maximumRootEventsPerDrain);

  private:
    explicit RootEventStream(std::shared_ptr<X11Connection::Implementation> implementation,
                             std::uint32_t addedMask) noexcept
        : implementation_(std::move(implementation)), addedMask_(addedMask) {}

    void releaseSubscription() noexcept;

    std::shared_ptr<X11Connection::Implementation> implementation_;
    std::uint32_t addedMask_;
    std::uint8_t randrFirstEvent_{0U};
    bool randrSupportsResourceChange_{false};
    bool randrEventsSelected_{false};
};

} // namespace prismdrake::x11
