#pragma once

#include <QRect>
#include <QString>
#include <QtTypes>

#include <memory>

struct xcb_connection_t;

namespace prismdrake::experiments {

class X11DockAdapter final {
public:
    X11DockAdapter();
    ~X11DockAdapter();

    X11DockAdapter(const X11DockAdapter &) = delete;
    X11DockAdapter &operator=(const X11DockAdapter &) = delete;

    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] bool applyBottomDockProperties(
        quint32 windowId,
        const QRect &logicalGeometry,
        qreal devicePixelRatio,
        QString *errorMessage = nullptr);

private:
    struct ConnectionDeleter {
        void operator()(xcb_connection_t *connection) const;
    };

    [[nodiscard]] quint32 internAtom(const char *name, QString *errorMessage) const;

    std::unique_ptr<xcb_connection_t, ConnectionDeleter> connection_;
};

} // namespace prismdrake::experiments
