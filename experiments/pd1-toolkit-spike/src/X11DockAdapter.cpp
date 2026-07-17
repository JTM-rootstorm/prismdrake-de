#include "X11DockAdapter.hpp"

#include <QByteArray>

#include <xcb/xcb.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace prismdrake::experiments {

void X11DockAdapter::ConnectionDeleter::operator()(xcb_connection_t *connection) const
{
    if (connection != nullptr) {
        xcb_disconnect(connection);
    }
}

X11DockAdapter::X11DockAdapter()
    : connection_(xcb_connect(nullptr, nullptr))
{
    if (connection_ != nullptr && xcb_connection_has_error(connection_.get()) != 0) {
        connection_.reset();
    }
}

X11DockAdapter::~X11DockAdapter() = default;

bool X11DockAdapter::isConnected() const
{
    return connection_ != nullptr;
}

bool X11DockAdapter::applyBottomDockProperties(
    quint32 windowId,
    const QRect &logicalGeometry,
    qreal devicePixelRatio,
    QString *errorMessage)
{
    if (!isConnected()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot connect isolated XCB dock adapter to DISPLAY");
        }
        return false;
    }
    if (windowId == XCB_WINDOW_NONE || logicalGeometry.isEmpty() || devicePixelRatio <= 0.0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot apply dock properties to invalid window geometry");
        }
        return false;
    }

    const xcb_atom_t windowType = internAtom("_NET_WM_WINDOW_TYPE", errorMessage);
    const xcb_atom_t dockType = internAtom("_NET_WM_WINDOW_TYPE_DOCK", errorMessage);
    const xcb_atom_t strut = internAtom("_NET_WM_STRUT", errorMessage);
    const xcb_atom_t strutPartial = internAtom("_NET_WM_STRUT_PARTIAL", errorMessage);
    if (windowType == XCB_ATOM_NONE || dockType == XCB_ATOM_NONE
        || strut == XCB_ATOM_NONE || strutPartial == XCB_ATOM_NONE) {
        return false;
    }

    const auto checkRequest = [this, errorMessage](
                                  xcb_void_cookie_t cookie,
                                  const QString &propertyName) {
        xcb_generic_error_t *requestError = xcb_request_check(connection_.get(), cookie);
        if (requestError == nullptr) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("X11 rejected property '%1' with error code %2")
                                .arg(propertyName)
                                .arg(requestError->error_code);
        }
        std::free(requestError);
        return false;
    };

    const xcb_void_cookie_t windowTypeCookie = xcb_change_property_checked(
        connection_.get(),
        XCB_PROP_MODE_REPLACE,
        windowId,
        windowType,
        XCB_ATOM_ATOM,
        32,
        1,
        &dockType);
    if (!checkRequest(windowTypeCookie, QStringLiteral("_NET_WM_WINDOW_TYPE"))) {
        return false;
    }

    const quint32 physicalHeight = static_cast<quint32>(
        std::lround(logicalGeometry.height() * devicePixelRatio));
    const quint32 physicalStartX = static_cast<quint32>(
        std::max(0L, std::lround(logicalGeometry.left() * devicePixelRatio)));
    const quint32 physicalEndX = static_cast<quint32>(
        std::max(0L, std::lround(logicalGeometry.right() * devicePixelRatio)));
    const std::array<quint32, 4> basicStrut{0, 0, 0, physicalHeight};
    const std::array<quint32, 12> partialStrut{
        0,
        0,
        0,
        physicalHeight,
        0,
        0,
        0,
        0,
        0,
        0,
        physicalStartX,
        physicalEndX,
    };

    const xcb_void_cookie_t strutCookie = xcb_change_property_checked(
        connection_.get(),
        XCB_PROP_MODE_REPLACE,
        windowId,
        strut,
        XCB_ATOM_CARDINAL,
        32,
        basicStrut.size(),
        basicStrut.data());
    if (!checkRequest(strutCookie, QStringLiteral("_NET_WM_STRUT"))) {
        return false;
    }

    const xcb_void_cookie_t partialStrutCookie = xcb_change_property_checked(
        connection_.get(),
        XCB_PROP_MODE_REPLACE,
        windowId,
        strutPartial,
        XCB_ATOM_CARDINAL,
        32,
        partialStrut.size(),
        partialStrut.data());
    if (!checkRequest(
            partialStrutCookie, QStringLiteral("_NET_WM_STRUT_PARTIAL"))) {
        return false;
    }
    xcb_flush(connection_.get());

    if (xcb_connection_has_error(connection_.get()) != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("XCB reported an error while applying dock properties");
        }
        return false;
    }
    return true;
}

quint32 X11DockAdapter::internAtom(const char *name, QString *errorMessage) const
{
    const std::size_t nameLength = std::strlen(name);
    if (nameLength > std::numeric_limits<std::uint16_t>::max()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("X11 atom name exceeds the protocol limit");
        }
        return XCB_ATOM_NONE;
    }
    const xcb_intern_atom_cookie_t cookie = xcb_intern_atom(
        connection_.get(), false, static_cast<std::uint16_t>(nameLength), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection_.get(), cookie, nullptr);
    if (reply == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Cannot intern required X11 atom '%1'")
                                .arg(QString::fromLatin1(name));
        }
        return XCB_ATOM_NONE;
    }

    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
}

} // namespace prismdrake::experiments
