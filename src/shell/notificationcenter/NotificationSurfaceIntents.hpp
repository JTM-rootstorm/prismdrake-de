#pragma once

#include "Result.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>

namespace prismdrake::shell::notificationcenter {

struct NotificationFocusOrigin final {
    std::uint64_t value{0U};

    friend bool operator==(const NotificationFocusOrigin &,
                           const NotificationFocusOrigin &) = default;
};

enum class NotificationSurfaceKind : std::uint8_t { center, transientStack };

struct NotificationSurfacePlacementIntent final {
    NotificationSurfaceKind kind{NotificationSurfaceKind::center};
    QString outputName;
    std::optional<NotificationFocusOrigin> focusOrigin;
    std::uint64_t topologyGeneration{0U};

    friend bool operator==(const NotificationSurfacePlacementIntent &,
                           const NotificationSurfacePlacementIntent &) = default;
};

/// Produces typed surface-placement and focus-return requests for the WP12 router.
class NotificationSurfaceIntentSource final : public QObject {
    Q_OBJECT

  public:
    explicit NotificationSurfaceIntentSource(QObject *parent = nullptr) : QObject(parent) {}

    [[nodiscard]] foundation::Result<void> requestCenter(QString outputName,
                                                         NotificationFocusOrigin focusOrigin);
    [[nodiscard]] foundation::Result<void> requestTransientStack(QString primaryOutputName,
                                                                 std::uint64_t topologyGeneration);
    [[nodiscard]] foundation::Result<void> requestFocusReturn(NotificationFocusOrigin focusOrigin);

  signals:
    void placementRequested(
        const prismdrake::shell::notificationcenter::NotificationSurfacePlacementIntent &intent);
    void focusReturnRequested(
        prismdrake::shell::notificationcenter::NotificationFocusOrigin focusOrigin);
};

} // namespace prismdrake::shell::notificationcenter

Q_DECLARE_METATYPE(prismdrake::shell::notificationcenter::NotificationFocusOrigin)
Q_DECLARE_METATYPE(prismdrake::shell::notificationcenter::NotificationSurfacePlacementIntent)
