#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace prismdrake::x11 {

class X11Connection;
class DockProperties;
class EwmhWindowRequests;
class PropertyReader;

/// Closed atom vocabulary used by the PD1 standards-only X11 adapter.
///
/// Callers cannot intern arbitrary names obtained from untrusted window metadata. Additions require
/// a reviewed source change and a corresponding explicit protocol use.
enum class AtomName : std::uint8_t {
    atom,
    cardinal,
    window,
    string,
    utf8_string,
    wm_name,
    wm_class,
    wm_hints,
    wm_transient_for,
    wm_protocols,
    wm_delete_window,
    wm_change_state,
    net_supported,
    net_supporting_wm_check,
    net_client_list,
    net_client_list_stacking,
    net_active_window,
    net_close_window,
    net_number_of_desktops,
    net_current_desktop,
    net_wm_desktop,
    net_wm_name,
    net_wm_pid,
    net_wm_icon,
    net_wm_window_type,
    net_wm_window_type_normal,
    net_wm_window_type_dialog,
    net_wm_window_type_utility,
    net_wm_window_type_toolbar,
    net_wm_window_type_menu,
    net_wm_window_type_splash,
    net_wm_window_type_dropdown_menu,
    net_wm_window_type_popup_menu,
    net_wm_window_type_tooltip,
    net_wm_window_type_notification,
    net_wm_window_type_combo,
    net_wm_window_type_dnd,
    net_wm_window_type_dock,
    net_wm_window_type_desktop,
    net_wm_strut,
    net_wm_strut_partial,
    net_wm_state,
    net_wm_state_hidden,
    net_wm_state_fullscreen,
    net_wm_state_modal,
    net_wm_state_skip_taskbar,
    net_wm_state_demands_attention,
    count,
};

/// Complete, immutable atom table stamped with its originating X11 connection.
///
/// The value may outlive that connection, but property reads accept it only with the same live
/// connection identity.
class AtomCache final {
  public:
    /// Interns every name in the closed vocabulary and fails if any reply is missing or invalid.
    [[nodiscard]] static foundation::Result<AtomCache> create(X11Connection &connection);

    /// Returns no value for the count sentinel or an invalid enum representation.
    [[nodiscard]] std::optional<AtomId> atom(AtomName name) const noexcept;

  private:
    friend class DockProperties;
    friend class EwmhWindowRequests;
    friend class PropertyReader;

    static constexpr std::size_t atomCount = static_cast<std::size_t>(AtomName::count);

    explicit AtomCache(std::array<std::optional<AtomId>, atomCount> atoms,
                       std::uint64_t connectionIdentity) noexcept
        : atoms_(atoms), connection_identity_(connectionIdentity) {}

    [[nodiscard]] bool belongsTo(std::uint64_t connectionIdentity) const noexcept {
        return connectionIdentity != 0U && connection_identity_ == connectionIdentity;
    }

    std::array<std::optional<AtomId>, atomCount> atoms_{};
    std::uint64_t connection_identity_;
};

} // namespace prismdrake::x11
