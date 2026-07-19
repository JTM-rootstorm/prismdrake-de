#include "NotificationRuntimeAdapter.hpp"

#include <limits>
#include <utility>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using quicksettings::CommandFailure;
using quicksettings::CommandResult;

[[nodiscard]] foundation::Error adapterError() {
    return {ErrorCode::io_error, "The shell notification adapter rejected an update.",
            "Retain the previous complete notification presentation and refetch notifyd state."};
}

} // namespace

NotificationRuntimeAdapter::NotificationRuntimeAdapter(
    quicksettings::QuickSettingsModel &quickSettings, AuthorityObserver authorityObserver,
    FailureObserver failureObserver, QObject *parent)
    : QObject(parent), quick_settings_(&quickSettings),
      authority_observer_(std::move(authorityObserver)),
      failure_observer_(std::move(failureObserver)), client_(this), presentation_(this) {
    quick_settings_->setDoNotDisturbControlPort(this);
    connect(&client_, &notificationcenter::NotificationCenterClient::snapshotChanged, this,
            &NotificationRuntimeAdapter::handleSnapshot);
    connect(&client_, &notificationcenter::NotificationCenterClient::stateChanged, this,
            &NotificationRuntimeAdapter::handleState);
    connect(&client_, &notificationcenter::NotificationCenterClient::mutationCompleted, this,
            &NotificationRuntimeAdapter::handleMutation);
    connect(&presentation_,
            &notificationcenter::NotificationCenterPresentation::doNotDisturbRequested, this,
            [this](bool enabled) {
                if (!pending_dnd_) {
                    auto requested = client_.setDoNotDisturb(enabled);
                    if (!requested) {
                        report(requested.error());
                    }
                }
            });
    connect(&presentation_,
            &notificationcenter::NotificationCenterPresentation::clearHistoryRequested, this,
            [this]() {
                auto requested = client_.clearHistory();
                if (!requested) {
                    report(requested.error());
                }
            });
    connect(&presentation_, &notificationcenter::NotificationCenterPresentation::dismissalRequested,
            this, [this](notificationcenter::NotificationRecordCommandIdentity identity) {
                auto requested = client_.dismiss(identity);
                if (!requested) {
                    report(requested.error());
                }
            });
    connect(&presentation_, &notificationcenter::NotificationCenterPresentation::actionRequested,
            this,
            [this](notificationcenter::NotificationRecordCommandIdentity identity,
                   const QString &actionId) {
                auto requested = client_.invokeAction(identity, actionId);
                if (!requested) {
                    report(requested.error());
                }
            });
}

NotificationRuntimeAdapter::~NotificationRuntimeAdapter() { stop(); }

Result<void> NotificationRuntimeAdapter::start() { return client_.start(); }

void NotificationRuntimeAdapter::stop() noexcept {
    client_.stop();
    loseOwner();
}

CommandResult
NotificationRuntimeAdapter::requestDoNotDisturb(bool enabled,
                                                quicksettings::CommandRequestId requestId) {
    if (!owner_available_) {
        return {CommandFailure::unavailable};
    }
    if (pending_dnd_ ||
        client_.activeMutation() != notificationcenter::NotificationCenterClient::Mutation::none) {
        return {CommandFailure::busy};
    }
    auto requested = client_.setDoNotDisturb(enabled);
    if (!requested) {
        return {requested.error().code == ErrorCode::cancelled ? CommandFailure::busy
                                                               : CommandFailure::failed};
    }
    pending_dnd_ = requestId;
    pending_dnd_confirmed_ = false;
    return {CommandFailure::none};
}

void NotificationRuntimeAdapter::handleSnapshot() {
    const auto snapshot = client_.currentSnapshot();
    if (!snapshot) {
        loseOwner();
        return;
    }
    auto generation = foundation::Generation::fromPublished(snapshot->generation);
    if (!generation) {
        report(generation.error());
        loseOwner();
        return;
    }
    if (!owner_available_) {
        if (owner_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            report(adapterError());
            return;
        }
        ++owner_epoch_;
        owner_available_ = true;
    }
    if (pending_dnd_ && pending_dnd_confirmed_) {
        auto completed = quick_settings_->completeDoNotDisturbRequest(
            *pending_dnd_, {CommandFailure::none, generation.value(), owner_epoch_});
        if (!completed) {
            report(completed.error());
        }
        pending_dnd_.reset();
        pending_dnd_confirmed_ = false;
    }
    auto applied = presentation_.applySnapshot(snapshot);
    if (!applied) {
        report(applied.error());
        return;
    }
    applied = quick_settings_->applyDoNotDisturbPublication(
        {owner_epoch_, generation.value(), snapshot->effectiveDoNotDisturb});
    if (!applied) {
        report(applied.error());
        return;
    }
    quick_settings_->setNotificationCenterAvailable(true);
    if (authority_observer_) {
        authority_observer_(composition::AuthoritySnapshot{
            composition::OwnerEpoch::create(owner_epoch_).value(),
            composition::OwnerGeneration::create(snapshot->generation).value()});
    }
}

void NotificationRuntimeAdapter::handleState() {
    if (client_.state() == notificationcenter::NotificationCenterClient::State::unavailable ||
        client_.state() == notificationcenter::NotificationCenterClient::State::failed ||
        client_.state() == notificationcenter::NotificationCenterClient::State::stopped) {
        loseOwner();
    }
    if (client_.state() == notificationcenter::NotificationCenterClient::State::failed &&
        client_.lastError()) {
        report(*client_.lastError());
    }
}

void NotificationRuntimeAdapter::handleMutation(
    notificationcenter::NotificationCenterClient::Mutation mutation, bool confirmed) {
    if (mutation != notificationcenter::NotificationCenterClient::Mutation::setDoNotDisturb ||
        !pending_dnd_) {
        return;
    }
    if (confirmed) {
        pending_dnd_confirmed_ = true;
        return;
    }
    auto completed = quick_settings_->completeDoNotDisturbRequest(
        *pending_dnd_, {CommandFailure::failed, {}, owner_epoch_});
    if (!completed) {
        report(completed.error());
    }
    pending_dnd_.reset();
    pending_dnd_confirmed_ = false;
}

void NotificationRuntimeAdapter::loseOwner() noexcept {
    if (!owner_available_ && !presentation_.serviceAvailable()) {
        return;
    }
    owner_available_ = false;
    pending_dnd_.reset();
    pending_dnd_confirmed_ = false;
    presentation_.clear();
    quick_settings_->clearNotificationOwner();
    quick_settings_->setNotificationCenterAvailable(false);
    if (authority_observer_) {
        authority_observer_(std::nullopt);
    }
}

void NotificationRuntimeAdapter::report(const foundation::Error &error) const {
    if (failure_observer_) {
        failure_observer_(error);
    }
}

} // namespace prismdrake::shell::runtime
