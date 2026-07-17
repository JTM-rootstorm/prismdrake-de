#include "AtomCache.hpp"

#include "X11Connection.hpp"

#include <array>
#include <cstdlib>
#include <memory>
#include <string_view>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::array<std::string_view, static_cast<std::size_t>(AtomName::count)> atomNames{
    "ATOM",
    "CARDINAL",
    "WINDOW",
    "STRING",
    "UTF8_STRING",
    "WM_NAME",
    "WM_CLASS",
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "_NET_SUPPORTED",
    "_NET_SUPPORTING_WM_CHECK",
    "_NET_CLIENT_LIST",
    "_NET_CLIENT_LIST_STACKING",
    "_NET_ACTIVE_WINDOW",
    "_NET_NUMBER_OF_DESKTOPS",
    "_NET_CURRENT_DESKTOP",
    "_NET_WM_DESKTOP",
    "_NET_WM_NAME",
    "_NET_WM_PID",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_DOCK",
    "_NET_WM_STRUT_PARTIAL",
    "_NET_WM_STATE",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_STATE_FULLSCREEN",
};

static_assert(atomNames.size() == static_cast<std::size_t>(AtomName::count));

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

using AtomReply = std::unique_ptr<xcb_intern_atom_reply_t, ReplyDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, ReplyDeleter>;

[[nodiscard]] Result<AtomCache> atomFailure() {
    return Result<AtomCache>::failure(
        {ErrorCode::io_error, "The fixed X11 atom table could not be initialized.",
         "Reconnect to the X11 server and retry atom initialization."});
}

} // namespace

Result<AtomCache> AtomCache::create(X11Connection &connection) {
    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    const auto connectionIdentity = connection.identity();
    if (native == nullptr || connectionIdentity == 0U || !connection.healthy()) {
        return atomFailure();
    }

    std::array<xcb_intern_atom_cookie_t, atomCount> cookies{};
    for (std::size_t index = 0U; index < atomNames.size(); ++index) {
        const auto name = atomNames[index];
        cookies[index] =
            xcb_intern_atom(native, 0U, static_cast<std::uint16_t>(name.size()), name.data());
    }

    std::array<std::optional<AtomId>, atomCount> atoms{};
    bool failed = false;
    for (std::size_t index = 0U; index < cookies.size(); ++index) {
        xcb_generic_error_t *rawError = nullptr;
        AtomReply reply{xcb_intern_atom_reply(native, cookies[index], &rawError)};
        ProtocolError error{rawError};
        if (!reply || error) {
            failed = true;
            continue;
        }
        auto atom = AtomId::fromProtocol(reply->atom);
        if (!atom) {
            failed = true;
            continue;
        }
        atoms[index] = atom.value();
    }

    if (failed || xcb_connection_has_error(native) != 0) {
        return atomFailure();
    }
    return Result<AtomCache>::success(AtomCache{atoms, connectionIdentity});
}

std::optional<AtomId> AtomCache::atom(AtomName name) const noexcept {
    const auto index = static_cast<std::size_t>(name);
    return index < atoms_.size() ? atoms_[index] : std::nullopt;
}

} // namespace prismdrake::x11
