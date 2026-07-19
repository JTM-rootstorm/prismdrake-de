#include "QuickSettingsModel.hpp"

#include <QThread>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace prismdrake::shell::quicksettings {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Error invalidState(std::string message) {
    return {ErrorCode::validation_error, std::move(message),
            "Retain the prior quick-settings state and publish one complete owner generation."};
}

[[nodiscard]] QString profileDisplayName(config::Profile profile) {
    switch (profile) {
    case config::Profile::lustre:
        return QuickSettingsModel::tr("Prismdrake Lustre");
    case config::Profile::forge:
        return QuickSettingsModel::tr("Prismdrake Forge");
    }
    return {};
}

[[nodiscard]] QString commandFailureDescription(CommandFailure failure) {
    switch (failure) {
    case CommandFailure::none:
        return {};
    case CommandFailure::unavailable:
        return QuickSettingsModel::tr("The service is unavailable.");
    case CommandFailure::busy:
        return QuickSettingsModel::tr("The service is busy. Try again shortly.");
    case CommandFailure::denied:
        return QuickSettingsModel::tr("The request was not authorized.");
    case CommandFailure::failed:
        return QuickSettingsModel::tr("The request could not be completed.");
    }
    return QuickSettingsModel::tr("The request could not be completed.");
}

} // namespace

QuickSettingAdapter::QuickSettingAdapter(QString kindId, QString title, QString description,
                                         QObject *parent)
    : QObject(parent), kind_id_(std::move(kindId)), title_(std::move(title)),
      description_(std::move(description)) {}

QString QuickSettingAdapter::stateId() const {
    switch (state_) {
    case AdapterState::available:
        return QStringLiteral("available");
    case AdapterState::unavailable:
        return QStringLiteral("unavailable");
    case AdapterState::enabled:
        return QStringLiteral("enabled");
    case AdapterState::disabled:
        return QStringLiteral("disabled");
    case AdapterState::pending:
        return QStringLiteral("pending");
    case AdapterState::error:
        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

QString QuickSettingAdapter::stateText() const {
    switch (state_) {
    case AdapterState::available:
        return tr("Available");
    case AdapterState::unavailable:
        return tr("Unavailable");
    case AdapterState::enabled:
        return tr("Enabled");
    case AdapterState::disabled:
        return tr("Disabled");
    case AdapterState::pending:
        return tr("Pending");
    case AdapterState::error:
        return tr("Error");
    }
    return tr("Error");
}

bool QuickSettingAdapter::available() const noexcept { return state_ != AdapterState::unavailable; }

bool QuickSettingAdapter::interactive() const noexcept {
    return state_ == AdapterState::available || state_ == AdapterState::enabled ||
           state_ == AdapterState::disabled || state_ == AdapterState::error;
}

void QuickSettingAdapter::publish(AdapterState state, QString description, QString valueText,
                                  bool checked, QString valueId) {
    if (state_ == state && description_ == description && value_text_ == valueText &&
        checked_ == checked && value_id_ == valueId) {
        return;
    }
    state_ = state;
    description_ = std::move(description);
    value_id_ = std::move(valueId);
    value_text_ = std::move(valueText);
    checked_ = checked;
    emit changed();
}

QuickSettingsModel::QuickSettingsModel(QObject *parent)
    : QObject(parent), profile_(QStringLiteral("profile"), tr("Desktop profile"),
                                tr("The settings service is unavailable."), this),
      do_not_disturb_(QStringLiteral("do_not_disturb"), tr("Do not disturb"),
                      tr("The notification service is unavailable."), this),
      notification_center_(QStringLiteral("notification_center"), tr("Notifications"),
                           tr("The notification center is unavailable."), this),
      logout_(QStringLiteral("logout"), tr("Log out"), tr("The session service is unavailable."),
              this),
      diagnostics_(QStringLiteral("diagnostics"), tr("Diagnostics"),
                   tr("No diagnostics summary is available."), this) {
    profile_.publish(AdapterState::unavailable, tr("The settings service is unavailable."));
    do_not_disturb_.publish(AdapterState::unavailable,
                            tr("The notification service is unavailable."));
    notification_center_.publish(AdapterState::unavailable,
                                 tr("The notification center is unavailable."));
    logout_.publish(AdapterState::unavailable, tr("The session service is unavailable."));
    diagnostics_.publish(AdapterState::unavailable, tr("No diagnostics summary is available."));
}

void QuickSettingsModel::setProfileControlPort(ProfileControlPort *port) noexcept {
    profile_port_ = port;
    publishProfileState();
}

void QuickSettingsModel::setDoNotDisturbControlPort(DoNotDisturbControlPort *port) noexcept {
    do_not_disturb_port_ = port;
    publishDoNotDisturbState();
}

void QuickSettingsModel::setNotificationCenterPort(NotificationCenterPort *port) noexcept {
    notification_center_port_ = port;
    publishNotificationCenterState();
}

void QuickSettingsModel::setLogoutControlPort(LogoutControlPort *port) noexcept {
    logout_port_ = port;
    publishLogoutState();
}

Result<void> QuickSettingsModel::applySettingsSnapshot(
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings settings state crossed QObject threads",
             "Queue the complete settings snapshot to the quick-settings owner thread."});
    }
    if (!snapshot ||
        snapshot->snapshotSchemaVersion != prismdrake::settings::runtimeSnapshotSchemaVersion ||
        snapshot->generation.value() == foundation::Generation::unpublishedValue ||
        profileDisplayName(snapshot->candidate.configuration.profile).isEmpty()) {
        return Result<void>::failure(invalidState("quick-settings settings state is invalid"));
    }
    if (settings_snapshot_) {
        if (snapshot->generation < settings_snapshot_->generation) {
            return Result<void>::failure({ErrorCode::cancelled,
                                          "quick-settings settings state is stale",
                                          "Retain the newer complete settings generation."});
        }
        if (snapshot->generation == settings_snapshot_->generation) {
            if (snapshot == settings_snapshot_) {
                return Result<void>::success();
            }
            return Result<void>::failure(
                invalidState("quick-settings settings generation has conflicting content"));
        }
    }

    settings_snapshot_ = std::move(snapshot);
    reconcilePendingProfile();
    return Result<void>::success();
}

void QuickSettingsModel::clearSettingsOwner() noexcept {
    settings_snapshot_.reset();
    pending_profile_.reset();
    publishProfileState();
}

Result<void> QuickSettingsModel::applyDoNotDisturbPublication(DoNotDisturbPublication publication) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings notification state crossed QObject threads",
             "Queue the complete notification state to the quick-settings owner thread."});
    }
    if (publication.ownerEpoch == 0U ||
        publication.generation.value() == foundation::Generation::unpublishedValue) {
        return Result<void>::failure(invalidState("quick-settings DND state is invalid"));
    }
    if (do_not_disturb_publication_ &&
        publication.ownerEpoch < do_not_disturb_publication_->ownerEpoch) {
        return Result<void>::failure({ErrorCode::cancelled,
                                      "quick-settings DND owner epoch is stale",
                                      "Retain the newer notification owner epoch."});
    }
    if (do_not_disturb_publication_ &&
        publication.ownerEpoch == do_not_disturb_publication_->ownerEpoch) {
        if (publication.generation < do_not_disturb_publication_->generation) {
            return Result<void>::failure({ErrorCode::cancelled, "quick-settings DND state is stale",
                                          "Retain the newer complete notification generation."});
        }
        if (publication.generation == do_not_disturb_publication_->generation) {
            if (publication == *do_not_disturb_publication_) {
                return Result<void>::success();
            }
            return Result<void>::failure(
                invalidState("quick-settings DND generation has conflicting content"));
        }
    }

    do_not_disturb_publication_ = publication;
    reconcilePendingDoNotDisturb();
    return Result<void>::success();
}

void QuickSettingsModel::clearNotificationOwner() noexcept {
    do_not_disturb_publication_.reset();
    pending_do_not_disturb_.reset();
    notification_center_available_ = false;
    notification_center_visible_ = false;
    notification_center_pending_ = false;
    publishDoNotDisturbState();
    publishNotificationCenterState();
}

Result<void> QuickSettingsModel::completeProfileRequest(CommandRequestId requestId,
                                                        CommandCompletion completion) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings profile completion crossed QObject threads",
             "Queue the typed completion to the quick-settings owner thread."});
    }
    if (!pending_profile_ || pending_profile_->id != requestId) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings profile completion is stale",
             "Discard the completion for the superseded adapter request."});
    }
    if (completion.failure != CommandFailure::none) {
        profile_.publish(commandFailureState(completion.failure),
                         commandFailureDescription(completion.failure));
        pending_profile_.reset();
        return Result<void>::success();
    }
    if (!completion.resultingGeneration || completion.ownerEpoch ||
        completion.resultingGeneration->value() == foundation::Generation::unpublishedValue) {
        return Result<void>::failure(invalidState("quick-settings profile completion is invalid"));
    }
    pending_profile_->resultingGeneration = completion.resultingGeneration;
    reconcilePendingProfile();
    return Result<void>::success();
}

Result<void> QuickSettingsModel::completeDoNotDisturbRequest(CommandRequestId requestId,
                                                             CommandCompletion completion) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings DND completion crossed QObject threads",
             "Queue the typed completion to the quick-settings owner thread."});
    }
    if (!pending_do_not_disturb_ || pending_do_not_disturb_->id != requestId) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "quick-settings DND completion is stale",
             "Discard the completion for the superseded adapter request."});
    }
    if (completion.failure != CommandFailure::none) {
        do_not_disturb_.publish(commandFailureState(completion.failure),
                                commandFailureDescription(completion.failure));
        pending_do_not_disturb_.reset();
        return Result<void>::success();
    }
    if (!completion.resultingGeneration || !completion.ownerEpoch || *completion.ownerEpoch == 0U ||
        completion.resultingGeneration->value() == foundation::Generation::unpublishedValue) {
        return Result<void>::failure(invalidState("quick-settings DND completion is invalid"));
    }
    pending_do_not_disturb_->ownerEpoch = completion.ownerEpoch;
    pending_do_not_disturb_->resultingGeneration = completion.resultingGeneration;
    reconcilePendingDoNotDisturb();
    return Result<void>::success();
}

void QuickSettingsModel::setNotificationCenterAvailable(bool available) noexcept {
    notification_center_available_ = available;
    if (!available) {
        notification_center_visible_ = false;
        notification_center_pending_ = false;
    }
    publishNotificationCenterState();
}

void QuickSettingsModel::observeNotificationCenterVisible(bool visible) noexcept {
    notification_center_visible_ = notification_center_available_ && visible;
    notification_center_pending_ = false;
    publishNotificationCenterState();
}

void QuickSettingsModel::setSessionAvailable(bool available) noexcept {
    session_available_ = available;
    if (!available) {
        logout_pending_ = false;
        session_state_ = SessionPresentationState::idle;
    }
    publishLogoutState();
}

void QuickSettingsModel::observeSessionState(SessionPresentationState state) noexcept {
    session_state_ = state;
    logout_pending_ = state != SessionPresentationState::idle;
    publishLogoutState();
}

void QuickSettingsModel::applyDiagnosticSummary(DiagnosticSummary summary) noexcept {
    switch (summary) {
    case DiagnosticSummary::unavailable:
        diagnostics_.publish(AdapterState::unavailable, tr("No diagnostics summary is available."));
        break;
    case DiagnosticSummary::healthy:
        diagnostics_.publish(AdapterState::enabled, tr("No active degradation reported."),
                             tr("Healthy"));
        break;
    case DiagnosticSummary::degraded:
        diagnostics_.publish(AdapterState::enabled,
                             tr("One or more optional capabilities are degraded."), tr("Degraded"));
        break;
    case DiagnosticSummary::failed:
        diagnostics_.publish(AdapterState::error, tr("A Prismdrake component reported a failure."),
                             tr("Error"));
        break;
    }
}

bool QuickSettingsModel::requestLustreProfile() { return requestProfile(config::Profile::lustre); }

bool QuickSettingsModel::requestForgeProfile() { return requestProfile(config::Profile::forge); }

bool QuickSettingsModel::requestProfile(config::Profile requested) {
    if (!settings_snapshot_ || profile_port_ == nullptr || profile_.pending()) {
        return false;
    }
    if (settings_snapshot_->candidate.configuration.profile == requested) {
        publishProfileState();
        return true;
    }
    const auto requestId = allocateRequestId();
    const auto result = profile_port_->requestProfile(requested, requestId);
    if (!result.accepted()) {
        profile_.publish(
            commandFailureState(result.failure), commandFailureDescription(result.failure),
            profileDisplayName(settings_snapshot_->candidate.configuration.profile), false,
            settings_snapshot_->candidate.configuration.profile == config::Profile::lustre
                ? QStringLiteral("lustre")
                : QStringLiteral("forge"));
        return false;
    }
    pending_profile_ = PendingProfileRequest{requestId, requested, std::nullopt};
    profile_.publish(AdapterState::pending, tr("Applying the complete profile generation."),
                     profileDisplayName(settings_snapshot_->candidate.configuration.profile), false,
                     settings_snapshot_->candidate.configuration.profile == config::Profile::lustre
                         ? QStringLiteral("lustre")
                         : QStringLiteral("forge"));
    return true;
}

bool QuickSettingsModel::requestDoNotDisturb(bool enabled) {
    if (!do_not_disturb_publication_ || do_not_disturb_port_ == nullptr ||
        do_not_disturb_.pending()) {
        return false;
    }
    if (do_not_disturb_publication_->effective == enabled) {
        publishDoNotDisturbState();
        return true;
    }
    const auto requestId = allocateRequestId();
    const auto result = do_not_disturb_port_->requestDoNotDisturb(enabled, requestId);
    if (!result.accepted()) {
        do_not_disturb_.publish(commandFailureState(result.failure),
                                commandFailureDescription(result.failure), {},
                                do_not_disturb_publication_->effective);
        return false;
    }
    pending_do_not_disturb_ =
        PendingDoNotDisturbRequest{requestId, enabled, std::nullopt, std::nullopt};
    do_not_disturb_.publish(AdapterState::pending,
                            tr("Waiting for a complete notification-state generation."), {},
                            do_not_disturb_publication_->effective);
    return true;
}

bool QuickSettingsModel::requestNotificationCenter() {
    if (!notification_center_available_ || notification_center_port_ == nullptr ||
        notification_center_pending_) {
        return false;
    }
    const auto result = notification_center_port_->requestOpen();
    if (!result.accepted()) {
        notification_center_.publish(commandFailureState(result.failure),
                                     commandFailureDescription(result.failure));
        return false;
    }
    notification_center_pending_ = true;
    publishNotificationCenterState();
    return true;
}

bool QuickSettingsModel::requestLogout() {
    if (!session_available_ || logout_port_ == nullptr || logout_pending_ ||
        session_state_ != SessionPresentationState::idle) {
        return false;
    }
    const auto result = logout_port_->requestLogout();
    if (!result.accepted()) {
        logout_.publish(commandFailureState(result.failure),
                        commandFailureDescription(result.failure));
        return false;
    }
    logout_pending_ = true;
    publishLogoutState();
    return true;
}

void QuickSettingsModel::publishProfileState() {
    if (!settings_snapshot_ || profile_port_ == nullptr) {
        profile_.publish(AdapterState::unavailable, tr("The settings service is unavailable."));
        return;
    }
    if (pending_profile_) {
        profile_.publish(
            AdapterState::pending, tr("Applying the complete profile generation."),
            profileDisplayName(settings_snapshot_->candidate.configuration.profile), false,
            settings_snapshot_->candidate.configuration.profile == config::Profile::lustre
                ? QStringLiteral("lustre")
                : QStringLiteral("forge"));
        return;
    }
    profile_.publish(AdapterState::available, tr("Choose the active Prismdrake profile."),
                     profileDisplayName(settings_snapshot_->candidate.configuration.profile), false,
                     settings_snapshot_->candidate.configuration.profile == config::Profile::lustre
                         ? QStringLiteral("lustre")
                         : QStringLiteral("forge"));
}

void QuickSettingsModel::publishDoNotDisturbState() {
    if (!do_not_disturb_publication_ || do_not_disturb_port_ == nullptr) {
        do_not_disturb_.publish(AdapterState::unavailable,
                                tr("The notification service is unavailable."));
        return;
    }
    if (pending_do_not_disturb_) {
        do_not_disturb_.publish(AdapterState::pending,
                                tr("Waiting for a complete notification-state generation."), {},
                                do_not_disturb_publication_->effective);
        return;
    }
    const bool effective = do_not_disturb_publication_->effective;
    do_not_disturb_.publish(effective ? AdapterState::enabled : AdapterState::disabled,
                            effective ? tr("Only critical interruptions are presented.")
                                      : tr("Normal notifications may be presented."),
                            {}, effective);
}

void QuickSettingsModel::publishNotificationCenterState() {
    if (!notification_center_available_ || notification_center_port_ == nullptr) {
        notification_center_.publish(AdapterState::unavailable,
                                     tr("The notification center is unavailable."));
        return;
    }
    if (notification_center_pending_) {
        notification_center_.publish(AdapterState::pending, tr("Opening the notification center."));
        return;
    }
    notification_center_.publish(AdapterState::available,
                                 notification_center_visible_
                                     ? tr("The notification center is open.")
                                     : tr("Open notification history and controls."),
                                 notification_center_visible_ ? tr("Open") : QString{});
}

void QuickSettingsModel::publishLogoutState() {
    if (!session_available_ || logout_port_ == nullptr) {
        logout_.publish(AdapterState::unavailable, tr("The session service is unavailable."));
        return;
    }
    if (session_state_ == SessionPresentationState::failed) {
        logout_.publish(AdapterState::error, tr("The logout transaction failed."));
        return;
    }
    if (logout_pending_ || session_state_ != SessionPresentationState::idle) {
        logout_.publish(AdapterState::pending,
                        session_state_ == SessionPresentationState::confirmationRequested
                            ? tr("Waiting for logout confirmation.")
                            : tr("The session is stopping."));
        return;
    }
    logout_.publish(AdapterState::available, tr("Request a confirmed session logout."));
}

CommandRequestId QuickSettingsModel::allocateRequestId() noexcept {
    const CommandRequestId allocated{next_request_id_};
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_request_id_ = 1U;
    } else {
        ++next_request_id_;
    }
    return allocated;
}

void QuickSettingsModel::reconcilePendingProfile() {
    if (!pending_profile_) {
        publishProfileState();
        return;
    }
    if (!settings_snapshot_ || !pending_profile_->resultingGeneration ||
        settings_snapshot_->generation < *pending_profile_->resultingGeneration) {
        publishProfileState();
        return;
    }

    if (settings_snapshot_->generation == *pending_profile_->resultingGeneration &&
        settings_snapshot_->candidate.configuration.profile != pending_profile_->requested) {
        const auto expected = profileDisplayName(pending_profile_->requested);
        const auto expectedId = pending_profile_->requested == config::Profile::lustre
                                    ? QStringLiteral("lustre")
                                    : QStringLiteral("forge");
        pending_profile_.reset();
        profile_.publish(AdapterState::error, tr("The requested profile was not applied."),
                         expected, false, expectedId);
        return;
    }

    pending_profile_.reset();
    publishProfileState();
}

void QuickSettingsModel::reconcilePendingDoNotDisturb() {
    if (!pending_do_not_disturb_) {
        publishDoNotDisturbState();
        return;
    }
    if (!do_not_disturb_publication_ || !pending_do_not_disturb_->ownerEpoch ||
        !pending_do_not_disturb_->resultingGeneration) {
        publishDoNotDisturbState();
        return;
    }

    const auto expectedOwner = *pending_do_not_disturb_->ownerEpoch;
    const auto expectedGeneration = *pending_do_not_disturb_->resultingGeneration;
    if (do_not_disturb_publication_->ownerEpoch < expectedOwner ||
        (do_not_disturb_publication_->ownerEpoch == expectedOwner &&
         do_not_disturb_publication_->generation < expectedGeneration)) {
        publishDoNotDisturbState();
        return;
    }

    if (do_not_disturb_publication_->ownerEpoch == expectedOwner &&
        do_not_disturb_publication_->generation == expectedGeneration &&
        do_not_disturb_publication_->effective != pending_do_not_disturb_->requested) {
        const bool effective = do_not_disturb_publication_->effective;
        pending_do_not_disturb_.reset();
        do_not_disturb_.publish(AdapterState::error,
                                tr("The requested do-not-disturb state was not applied."), {},
                                effective);
        return;
    }

    pending_do_not_disturb_.reset();
    publishDoNotDisturbState();
}

AdapterState QuickSettingsModel::commandFailureState(CommandFailure failure) noexcept {
    return failure == CommandFailure::unavailable ? AdapterState::unavailable : AdapterState::error;
}

} // namespace prismdrake::shell::quicksettings
