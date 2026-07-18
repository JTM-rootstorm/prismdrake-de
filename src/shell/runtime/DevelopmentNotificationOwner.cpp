#include "DevelopmentNotificationOwner.hpp"

#include <QByteArray>
#include <QCoreApplication>

#include <chrono>
#include <new>
#include <string>
#include <utility>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using prismdrake::notifications::NotificationTimeout;
using prismdrake::notifications::NotificationTimeoutKind;
using prismdrake::notifications::SyntheticNotificationAction;
using prismdrake::notifications::SyntheticNotificationInput;

constexpr auto translationContext = "DevelopmentNotificationOwner";
constexpr auto acknowledgeActionId = "acknowledge";

[[nodiscard]] std::string translatedUtf8(const char *sourceText) {
    const QByteArray utf8 = QCoreApplication::translate(translationContext, sourceText).toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

[[nodiscard]] std::string utf8(const QString &value) {
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

DevelopmentNotificationOwner::DevelopmentNotificationOwner(
    prismdrake::notifications::SyntheticNotificationModel model)
    : model_(std::move(model)) {}

Result<std::unique_ptr<DevelopmentNotificationOwner>>
DevelopmentNotificationOwner::create(std::shared_ptr<const foundation::MonotonicClock> clock) {
    auto model = prismdrake::notifications::SyntheticNotificationModel::create(std::move(clock));
    if (!model) {
        return Result<std::unique_ptr<DevelopmentNotificationOwner>>::failure(model.error());
    }

    try {
        return Result<std::unique_ptr<DevelopmentNotificationOwner>>::success(
            std::unique_ptr<DevelopmentNotificationOwner>(
                new DevelopmentNotificationOwner(std::move(model).value())));
    } catch (const std::bad_alloc &) {
        return Result<std::unique_ptr<DevelopmentNotificationOwner>>::failure(
            {ErrorCode::too_large, "The development notification owner could not be allocated.",
             "Reduce memory pressure before restarting prismdrake-shell."});
    }
}

Result<void> DevelopmentNotificationOwner::publishFixture() {
    SyntheticNotificationInput input;
    input.replacementId = fixture_id_;
    input.summary = translatedUtf8("Prismdrake test notification");
    input.body =
        translatedUtf8("This bounded synthetic card demonstrates notification presentation.");
    input.applicationName = translatedUtf8("Prismdrake Development Harness");
    input.applicationId = "org.prismdrake.development-harness";
    input.actions = {
        SyntheticNotificationAction{acknowledgeActionId, translatedUtf8("Acknowledge"), true}};
    input.timeout =
        NotificationTimeout{NotificationTimeoutKind::never, std::chrono::milliseconds::zero()};

    auto published = model_.upsert(std::move(input));
    if (!published) {
        return Result<void>::failure(published.error());
    }
    fixture_id_ = published.value().id;
    return apply(std::move(published).value().snapshot);
}

Result<void>
DevelopmentNotificationOwner::activate(prismdrake::notifications::NotificationId notificationId,
                                       foundation::Generation contentGeneration,
                                       const QString &actionId) {
    auto activated = model_.activateAction(notificationId, contentGeneration, utf8(actionId));
    if (!activated) {
        return Result<void>::failure(activated.error());
    }
    if (activated.value().actionId != acknowledgeActionId) {
        return Result<void>::failure({ErrorCode::unsupported,
                                      "The development notification action is unsupported.",
                                      "Use the current fixed synthetic notification affordance."});
    }
    return dismiss(notificationId, contentGeneration);
}

Result<void>
DevelopmentNotificationOwner::dismiss(prismdrake::notifications::NotificationId notificationId,
                                      foundation::Generation contentGeneration) {
    auto dismissed = model_.dismiss(notificationId, contentGeneration);
    if (!dismissed) {
        return Result<void>::failure(dismissed.error());
    }
    fixture_id_.reset();
    return apply(std::move(dismissed).value().snapshot);
}

Result<void> DevelopmentNotificationOwner::apply(
    std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot) {
    auto applied = presentation_.applySnapshot(std::move(snapshot));
    if (!applied) {
        return Result<void>::failure(applied.error());
    }
    return Result<void>::success();
}

} // namespace prismdrake::shell::runtime
