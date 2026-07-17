#pragma once

#include "AtomCache.hpp"
#include "Result.hpp"
#include "TaskModel.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace prismdrake::x11 {

class X11Connection;

inline constexpr std::size_t maximumTaskRefreshAttempts = 2U;

/// Connection-proven, bounded reader for one WM-owned EWMH task snapshot.
///
/// The source mirrors root and client properties only. It selects PropertyChange
/// on advertised clients so the connection's sole RootEventStream can request
/// later refreshes, but it never changes focus, stacking, or window state.
class EwmhTaskSource final {
  public:
    [[nodiscard]] static foundation::Result<EwmhTaskSource> create(X11Connection &connection);

    EwmhTaskSource(const EwmhTaskSource &) = delete;
    EwmhTaskSource &operator=(const EwmhTaskSource &) = delete;
    EwmhTaskSource(EwmhTaskSource &&other) noexcept;
    EwmhTaskSource &operator=(EwmhTaskSource &&) = delete;

    /// Reads one coherent root snapshot and one bounded client observation for
    /// every advertised XID. Torn root observations are retried once.
    [[nodiscard]] foundation::Result<TaskModelObservation> refresh(X11Connection &connection);

    /// Invalidates one observed incarnation after the sole event stream reports
    /// a create or destroy transition for that XID.
    void invalidateClient(WindowId window) noexcept;

  private:
    [[nodiscard]] static foundation::Result<bool>
    selectClientPropertyEvents(X11Connection &connection, WindowId window);

    EwmhTaskSource(AtomCache atoms, std::uint64_t connectionIdentity) noexcept
        : atoms_(std::move(atoms)), connection_identity_(connectionIdentity) {}

    AtomCache atoms_;
    std::uint64_t connection_identity_;
    std::optional<WindowId> current_owner_;
    std::map<WindowId::Value, WindowIncarnationId> incarnations_;
    std::uint64_t next_incarnation_{1U};
};

} // namespace prismdrake::x11
