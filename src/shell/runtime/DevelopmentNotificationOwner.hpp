#pragma once

#include "MonotonicClock.hpp"
#include "NotificationModel.hpp"
#include "NotificationPresentationModel.hpp"
#include "Result.hpp"

#include <QString>

#include <memory>
#include <optional>

namespace prismdrake::shell::runtime {

/// Development-only owner for the installed PD1 synthetic-notification demonstration.
///
/// This owner performs no D-Bus work and never claims the freedesktop notification service. It
/// keeps the bounded synthetic model and passive presentation mirror alive independently from a
/// settings-owned QML view epoch, so a validated card can be replayed after surface reconstruction.
class DevelopmentNotificationOwner final {
  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<DevelopmentNotificationOwner>>
    create(std::shared_ptr<const foundation::MonotonicClock> clock);

    DevelopmentNotificationOwner(const DevelopmentNotificationOwner &) = delete;
    DevelopmentNotificationOwner &operator=(const DevelopmentNotificationOwner &) = delete;

    [[nodiscard]] shell::notifications::NotificationPresentationModel *presentation() noexcept {
        return &presentation_;
    }
    [[nodiscard]] const shell::notifications::NotificationPresentationModel *
    presentation() const noexcept {
        return &presentation_;
    }
    [[nodiscard]] bool hasCard() const noexcept { return presentation_.rowCount() != 0; }

    /// Publishes or replaces the one fixed, non-expiring development card.
    [[nodiscard]] foundation::Result<void> publishFixture();

    /// Validates the exact typed action identity and dismisses the acknowledged fixture.
    [[nodiscard]] foundation::Result<void>
    activate(prismdrake::notifications::NotificationId notificationId,
             foundation::Generation contentGeneration, const QString &actionId);

    /// Dismisses only the exact current card identity and content generation.
    [[nodiscard]] foundation::Result<void>
    dismiss(prismdrake::notifications::NotificationId notificationId,
            foundation::Generation contentGeneration);

  private:
    explicit DevelopmentNotificationOwner(
        prismdrake::notifications::SyntheticNotificationModel model);

    [[nodiscard]] foundation::Result<void>
    apply(std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot);

    prismdrake::notifications::SyntheticNotificationModel model_;
    shell::notifications::NotificationPresentationModel presentation_;
    std::optional<prismdrake::notifications::NotificationId> fixture_id_;
};

} // namespace prismdrake::shell::runtime
