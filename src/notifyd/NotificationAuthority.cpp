#include "NotificationAuthority.hpp"

#include "NotificationHistory.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace prismdrake::notifyd {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::MonotonicClock;
using foundation::Result;
using notifications::HistoryMode;
using notifications::ServiceNotificationInput;
using notifications::ServiceTimeoutKind;
using notifications::ServiceUrgency;

[[nodiscard]] Error authorityError(ErrorCode code, std::string message) {
    return {code, std::move(message), "Retain the prior complete notification state."};
}

[[nodiscard]] Result<std::optional<MonotonicClock::TimePoint>>
expiryFor(const ServiceNotificationInput &input, std::chrono::milliseconds defaultTimeout,
          MonotonicClock::TimePoint now) {
    std::chrono::milliseconds duration{0};
    switch (input.timeout.kind) {
    case ServiceTimeoutKind::never:
        return Result<std::optional<MonotonicClock::TimePoint>>::success(std::nullopt);
    case ServiceTimeoutKind::configured_default:
        duration = defaultTimeout;
        break;
    case ServiceTimeoutKind::explicit_timeout:
        duration = input.timeout.duration;
        break;
    }
    if (duration == std::chrono::milliseconds::zero()) {
        return Result<std::optional<MonotonicClock::TimePoint>>::success(std::nullopt);
    }
    const auto converted = std::chrono::duration_cast<MonotonicClock::Duration>(duration);
    if (converted <= MonotonicClock::Duration::zero() ||
        now > MonotonicClock::TimePoint::max() - converted) {
        return Result<std::optional<MonotonicClock::TimePoint>>::failure(authorityError(
            ErrorCode::too_large, "The notification expiration deadline cannot be represented."));
    }
    return Result<std::optional<MonotonicClock::TimePoint>>::success(now + converted);
}

} // namespace

NotificationAuthority::NotificationAuthority(std::shared_ptr<const MonotonicClock> clock,
                                             AuthorityOptions options,
                                             std::vector<NotificationRecord> records,
                                             std::uint64_t nextRecordId) noexcept
    : clock_(std::move(clock)), history_mode_(options.historyMode),
      default_timeout_(options.defaultTimeout),
      configured_do_not_disturb_(options.configuredDoNotDisturb),
      notifications_enabled_(options.notificationsEnabled),
      persistent_history_path_(std::move(options.persistentHistoryPath)),
      records_(std::move(records)), next_record_id_(nextRecordId) {}

Result<NotificationAuthority>
NotificationAuthority::create(std::shared_ptr<const MonotonicClock> clock,
                              AuthorityOptions options) {
    if (!clock) {
        return Result<NotificationAuthority>::failure(authorityError(
            ErrorCode::invalid_argument, "A monotonic notification clock is required."));
    }
    if (options.defaultTimeout < std::chrono::milliseconds::zero() ||
        options.defaultTimeout > notifications::maximumServiceTimeout) {
        return Result<NotificationAuthority>::failure(authorityError(
            ErrorCode::invalid_argument, "The configured notification timeout is invalid."));
    }
    if (options.historyMode == HistoryMode::persistent && !options.persistentHistoryPath) {
        return Result<NotificationAuthority>::failure(authorityError(
            ErrorCode::invalid_argument, "Persistent notification history requires an XDG path."));
    }

    std::vector<NotificationRecord> records;
    std::uint64_t nextRecordId = 1U;
    if (options.historyMode == HistoryMode::persistent && options.persistentHistoryPath) {
        auto loaded = loadNotificationHistory(*options.persistentHistoryPath);
        if (loaded) {
            records = std::move(loaded).value();
            for (const auto &record : records) {
                if (record.recordId == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<NotificationAuthority>::failure(
                        authorityError(ErrorCode::too_large,
                                       "The notification record identifier space is exhausted."));
                }
                nextRecordId = std::max(nextRecordId, record.recordId + 1U);
            }
        } else if (loaded.error().code != ErrorCode::not_found) {
            // Malformed or inaccessible history is retained on disk. The live
            // service starts from an empty bounded history rather than failing
            // the complete desktop session.
            records.clear();
        }
    }
    return Result<NotificationAuthority>::success(NotificationAuthority{
        std::move(clock), std::move(options), std::move(records), nextRecordId});
}

Result<std::uint32_t> NotificationAuthority::allocateNotificationId() {
    auto candidate = next_notification_id_ == 0U ? 1U : next_notification_id_;
    for (std::size_t attempt = 0U; attempt <= records_.size(); ++attempt) {
        const bool collision = std::ranges::any_of(records_, [candidate](const auto &record) {
            return record.notificationId && *record.notificationId == candidate;
        });
        const auto following =
            candidate == std::numeric_limits<std::uint32_t>::max() ? 1U : candidate + 1U;
        if (!collision) {
            next_notification_id_ = following;
            return Result<std::uint32_t>::success(candidate);
        }
        candidate = following;
    }
    return Result<std::uint32_t>::failure(authorityError(
        ErrorCode::too_large, "The live notification identifier space is exhausted."));
}

Result<std::uint64_t> NotificationAuthority::allocateRecordId() {
    if (next_record_id_ == 0U) {
        return Result<std::uint64_t>::failure(authorityError(
            ErrorCode::too_large, "The notification record identifier space is exhausted."));
    }
    const auto allocated = next_record_id_;
    if (next_record_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_record_id_ = 0U;
    } else {
        ++next_record_id_;
    }
    return Result<std::uint64_t>::success(allocated);
}

Result<void> NotificationAuthority::ensurePublishable() const {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure(authorityError(
            ErrorCode::too_large, "The notification state generation is exhausted."));
    }
    return Result<void>::success();
}

Result<void> NotificationAuthority::publish() {
    auto publishable = ensurePublishable();
    if (!publishable) {
        return publishable;
    }
    ++generation_;
    return Result<void>::success();
}

void NotificationAuthority::makeCapacityForOne() {
    while (records_.size() >= notifications::maximumServiceRecords) {
        const auto closed = std::ranges::find_if(
            records_, [](const NotificationRecord &record) { return !record.notificationId; });
        if (closed == records_.end()) {
            return;
        }
        records_.erase(closed);
    }
}

bool NotificationAuthority::effectiveDoNotDisturb() const noexcept {
    return runtime_do_not_disturb_override_.value_or(configured_do_not_disturb_);
}

void NotificationAuthority::refreshTransientPresentation(
    NotificationRecord &record) const noexcept {
    record.transientPresentation =
        record.notificationId.has_value() && notifications_enabled_ &&
        (!effectiveDoNotDisturb() || record.urgency == ServiceUrgency::critical);
}

std::vector<NotificationRecord>::iterator
NotificationAuthority::findRecord(std::uint64_t recordId) {
    return std::ranges::find(records_, recordId, &NotificationRecord::recordId);
}

std::vector<NotificationRecord>::const_iterator
NotificationAuthority::findRecord(std::uint64_t recordId) const {
    return std::ranges::find(records_, recordId, &NotificationRecord::recordId);
}

Result<NotifyOutcome> NotificationAuthority::notify(ServiceNotificationInput input) {
    auto valid = notifications::validateServiceNotificationInput(input);
    if (!valid) {
        return Result<NotifyOutcome>::failure(valid.error());
    }
    auto expiry = expiryFor(input, default_timeout_, clock_->now());
    if (!expiry) {
        return Result<NotifyOutcome>::failure(expiry.error());
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<NotifyOutcome>::failure(publishable.error());
    }

    if (input.replacesId != 0U) {
        const auto existing =
            std::ranges::find_if(records_, [&input](const NotificationRecord &record) {
                return record.notificationId && *record.notificationId == input.replacesId;
            });
        if (existing != records_.end()) {
            if (existing->contentGeneration == std::numeric_limits<std::uint64_t>::max()) {
                return Result<NotifyOutcome>::failure(authorityError(
                    ErrorCode::too_large, "The notification content generation is exhausted."));
            }
            const auto nextContentGeneration = existing->contentGeneration + 1U;
            existing->contentGeneration = nextContentGeneration;
            existing->applicationName = std::move(input.applicationName);
            existing->desktopFileId = std::move(input.desktopFileId);
            existing->summary = std::move(input.summary);
            existing->body = std::move(input.body);
            existing->urgency = input.urgency;
            existing->receivedAtUnixMilliseconds = input.receivedAtUnixMilliseconds;
            existing->dismissible = input.dismissible;
            existing->transientHint = input.transientHint;
            existing->actions = std::move(input.actions);
            for (auto &action : existing->actions) {
                action.enabled = true;
            }
            existing->sender = std::move(input.sender);
            existing->expiresAt = expiry.value();
            refreshTransientPresentation(*existing);
            auto published = publish();
            if (!published) {
                return Result<NotifyOutcome>::failure(published.error());
            }
            return Result<NotifyOutcome>::success({*existing->notificationId, existing->recordId,
                                                   existing->contentGeneration, generation_, true});
        }
    }

    if (next_record_id_ == 0U) {
        return Result<NotifyOutcome>::failure(authorityError(
            ErrorCode::too_large, "The notification identifier space is exhausted."));
    }
    makeCapacityForOne();
    if (records_.size() >= notifications::maximumServiceRecords) {
        return Result<NotifyOutcome>::failure(
            authorityError(ErrorCode::too_large, "The live notification capacity is exhausted."));
    }
    auto notificationId = input.replacesId == 0U ? allocateNotificationId()
                                                 : Result<std::uint32_t>::success(input.replacesId);
    auto recordId = allocateRecordId();
    if (!recordId) {
        return Result<NotifyOutcome>::failure(recordId.error());
    }

    NotificationRecord record{
        recordId.value(),
        1U,
        notificationId.value(),
        std::move(input.applicationName),
        std::move(input.desktopFileId),
        std::move(input.summary),
        std::move(input.body),
        input.urgency,
        input.receivedAtUnixMilliseconds,
        input.dismissible,
        false,
        input.transientHint,
        std::move(input.actions),
        std::move(input.sender),
        expiry.value(),
    };
    for (auto &action : record.actions) {
        action.enabled = true;
    }
    refreshTransientPresentation(record);
    records_.push_back(std::move(record));
    auto published = publish();
    if (!published) {
        records_.pop_back();
        return Result<NotifyOutcome>::failure(published.error());
    }
    return Result<NotifyOutcome>::success(
        {notificationId.value(), recordId.value(), 1U, generation_, false});
}

Result<void> NotificationAuthority::persistIfNeeded() const {
    if (history_mode_ != HistoryMode::persistent || !persistent_history_path_) {
        return Result<void>::success();
    }
    return writeNotificationHistory(*persistent_history_path_, records_);
}

Result<void>
NotificationAuthority::persistCandidate(const std::vector<NotificationRecord> &records) const {
    if (history_mode_ != HistoryMode::persistent || !persistent_history_path_) {
        return Result<void>::success();
    }
    return writeNotificationHistory(*persistent_history_path_, records);
}

Result<CloseOutcome> NotificationAuthority::close(std::uint32_t notificationId,
                                                  CloseReason reason) {
    const auto record =
        std::ranges::find_if(records_, [notificationId](const NotificationRecord &item) {
            return item.notificationId && *item.notificationId == notificationId;
        });
    if (record == records_.end()) {
        return Result<CloseOutcome>::failure(
            authorityError(ErrorCode::not_found, "The live notification does not exist."));
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<CloseOutcome>::failure(publishable.error());
    }
    CloseEvent event{notificationId, reason, record->sender};

    record->notificationId.reset();
    record->sender.reset();
    record->expiresAt.reset();
    record->transientPresentation = false;
    for (auto &action : record->actions) {
        action.enabled = false;
    }
    if (history_mode_ == HistoryMode::disabled || record->transientHint) {
        records_.erase(record);
    }
    auto published = publish();
    if (!published) {
        return Result<CloseOutcome>::failure(published.error());
    }
    if (!persistIfNeeded()) {
        persistence_degraded_ = true;
    }
    return Result<CloseOutcome>::success({generation_, std::move(event)});
}

Result<MutationOutcome> NotificationAuthority::advanceTimeouts() {
    std::vector<std::size_t> expiredIndices;
    const auto now = clock_->now();
    for (std::size_t index = 0U; index < records_.size(); ++index) {
        const auto &record = records_[index];
        if (record.notificationId && record.expiresAt && *record.expiresAt <= now) {
            expiredIndices.push_back(index);
        }
    }
    if (expiredIndices.empty()) {
        return Result<MutationOutcome>::success({generation_, false, {}});
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<MutationOutcome>::failure(publishable.error());
    }

    MutationOutcome outcome{generation_, false, {}};
    outcome.closeEvents.reserve(expiredIndices.size());
    for (const auto index : expiredIndices) {
        auto &record = records_[index];
        outcome.closeEvents.push_back(
            {*record.notificationId, CloseReason::expired, record.sender});
        record.notificationId.reset();
        record.sender.reset();
        record.expiresAt.reset();
        record.transientPresentation = false;
        for (auto &action : record.actions) {
            action.enabled = false;
        }
    }
    std::erase_if(records_, [this](const NotificationRecord &record) {
        return !record.notificationId &&
               (history_mode_ == HistoryMode::disabled || record.transientHint);
    });
    auto published = publish();
    if (!published) {
        return Result<MutationOutcome>::failure(published.error());
    }
    if (!persistIfNeeded()) {
        persistence_degraded_ = true;
    }
    outcome.stateGeneration = generation_;
    outcome.published = true;
    return Result<MutationOutcome>::success(std::move(outcome));
}

Result<MutationOutcome> NotificationAuthority::senderDisappeared(std::string_view sender) {
    const bool visibleChange =
        std::ranges::any_of(records_, [sender](const NotificationRecord &record) {
            return record.sender && *record.sender == sender &&
                   std::ranges::any_of(record.actions, &notifications::ServiceAction::enabled);
        });
    if (visibleChange) {
        auto publishable = ensurePublishable();
        if (!publishable) {
            return Result<MutationOutcome>::failure(publishable.error());
        }
    }
    for (auto &record : records_) {
        if (!record.sender || *record.sender != sender) {
            continue;
        }
        record.sender.reset();
        for (auto &action : record.actions) {
            action.enabled = false;
        }
    }
    if (!visibleChange) {
        return Result<MutationOutcome>::success({generation_, false, {}});
    }
    auto published = publish();
    if (!published) {
        return Result<MutationOutcome>::failure(published.error());
    }
    return Result<MutationOutcome>::success({generation_, true, {}});
}

Result<MutationOutcome> NotificationAuthority::dismiss(std::uint64_t recordId,
                                                       std::uint64_t contentGeneration) {
    auto record = findRecord(recordId);
    if (record == records_.end() || record->contentGeneration != contentGeneration) {
        return Result<MutationOutcome>::failure(
            authorityError(ErrorCode::cancelled, "The notification record is stale."));
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<MutationOutcome>::failure(publishable.error());
    }
    MutationOutcome outcome{generation_, false, {}};
    if (record->notificationId) {
        outcome.closeEvents.push_back(
            {*record->notificationId, CloseReason::dismissed, record->sender});
    }
    const auto offset = static_cast<std::size_t>(std::distance(records_.begin(), record));
    auto candidate = records_;
    auto &dismissed = candidate[offset];
    dismissed.notificationId.reset();
    dismissed.sender.reset();
    dismissed.expiresAt.reset();
    dismissed.transientPresentation = false;
    for (auto &action : dismissed.actions) {
        action.enabled = false;
    }
    if (history_mode_ == HistoryMode::disabled || dismissed.transientHint) {
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    auto persisted = persistCandidate(candidate);
    if (!persisted) {
        return Result<MutationOutcome>::failure(persisted.error());
    }
    records_ = std::move(candidate);
    auto published = publish();
    if (!published) {
        return Result<MutationOutcome>::failure(published.error());
    }
    outcome.stateGeneration = generation_;
    outcome.published = true;
    return Result<MutationOutcome>::success(std::move(outcome));
}

Result<ActionEvent> NotificationAuthority::invokeAction(std::uint64_t recordId,
                                                        std::uint64_t contentGeneration,
                                                        std::string_view actionId) const {
    if (!notifications::isValidServiceActionId(actionId)) {
        return Result<ActionEvent>::failure(authorityError(
            ErrorCode::invalid_argument, "The notification action identifier is invalid."));
    }
    const auto record = findRecord(recordId);
    if (record == records_.end() || record->contentGeneration != contentGeneration) {
        return Result<ActionEvent>::failure(
            authorityError(ErrorCode::cancelled, "The notification record is stale."));
    }
    if (!record->notificationId || !record->sender) {
        return Result<ActionEvent>::failure(
            authorityError(ErrorCode::not_found, "The notification action is unavailable."));
    }
    const auto action =
        std::ranges::find(record->actions, actionId, &notifications::ServiceAction::id);
    if (action == record->actions.end() || !action->enabled) {
        return Result<ActionEvent>::failure(
            authorityError(ErrorCode::not_found, "The notification action is unavailable."));
    }
    return Result<ActionEvent>::success(
        {*record->notificationId, std::string{actionId}, *record->sender});
}

Result<std::uint64_t> NotificationAuthority::setDoNotDisturb(bool enabled) {
    if (effectiveDoNotDisturb() == enabled) {
        return Result<std::uint64_t>::success(generation_);
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<std::uint64_t>::failure(publishable.error());
    }
    runtime_do_not_disturb_override_ = enabled;
    for (auto &record : records_) {
        refreshTransientPresentation(record);
    }
    auto published = publish();
    if (!published) {
        return Result<std::uint64_t>::failure(published.error());
    }
    return Result<std::uint64_t>::success(generation_);
}

Result<std::uint64_t> NotificationAuthority::clearHistory() {
    auto candidate = records_;
    std::erase_if(candidate,
                  [](const NotificationRecord &record) { return !record.notificationId; });
    if (candidate.size() == records_.size()) {
        return Result<std::uint64_t>::success(generation_);
    }
    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<std::uint64_t>::failure(publishable.error());
    }
    auto persisted = persistCandidate(candidate);
    if (!persisted) {
        return Result<std::uint64_t>::failure(persisted.error());
    }
    records_ = std::move(candidate);
    auto published = publish();
    if (!published) {
        return Result<std::uint64_t>::failure(published.error());
    }
    return Result<std::uint64_t>::success(generation_);
}

Result<std::uint64_t> NotificationAuthority::applySettings(SettingsInput settings) {
    if (settings.defaultTimeout < std::chrono::milliseconds::zero() ||
        settings.defaultTimeout > notifications::maximumServiceTimeout ||
        (settings.historyMode == HistoryMode::persistent && !persistent_history_path_)) {
        return Result<std::uint64_t>::failure(authorityError(
            ErrorCode::invalid_argument, "The notification settings generation is invalid."));
    }
    const bool changed = history_mode_ != settings.historyMode ||
                         default_timeout_ != settings.defaultTimeout ||
                         configured_do_not_disturb_ != settings.configuredDoNotDisturb ||
                         notifications_enabled_ != settings.notificationsEnabled ||
                         runtime_do_not_disturb_override_.has_value();
    if (!changed) {
        return Result<std::uint64_t>::success(generation_);
    }

    auto publishable = ensurePublishable();
    if (!publishable) {
        return Result<std::uint64_t>::failure(publishable.error());
    }

    auto candidate = records_;
    if (settings.historyMode == HistoryMode::disabled) {
        std::erase_if(candidate,
                      [](const NotificationRecord &record) { return !record.notificationId; });
    }
    if ((history_mode_ == HistoryMode::persistent ||
         settings.historyMode == HistoryMode::persistent) &&
        persistent_history_path_) {
        auto persisted = writeNotificationHistory(*persistent_history_path_, candidate);
        if (!persisted) {
            persistence_degraded_ = true;
        }
    }

    history_mode_ = settings.historyMode;
    default_timeout_ = settings.defaultTimeout;
    configured_do_not_disturb_ = settings.configuredDoNotDisturb;
    notifications_enabled_ = settings.notificationsEnabled;
    runtime_do_not_disturb_override_.reset();
    records_ = std::move(candidate);
    for (auto &record : records_) {
        refreshTransientPresentation(record);
    }
    auto published = publish();
    if (!published) {
        return Result<std::uint64_t>::failure(published.error());
    }
    return Result<std::uint64_t>::success(generation_);
}

std::optional<MonotonicClock::TimePoint> NotificationAuthority::nextExpiry() const noexcept {
    std::optional<MonotonicClock::TimePoint> earliest;
    for (const auto &record : records_) {
        if (record.notificationId && record.expiresAt &&
            (!earliest || *record.expiresAt < *earliest)) {
            earliest = record.expiresAt;
        }
    }
    return earliest;
}

Result<void> NotificationAuthority::flushHistory() const { return persistIfNeeded(); }

bool NotificationAuthority::takePersistenceDegradation() noexcept {
    return std::exchange(persistence_degraded_, false);
}

} // namespace prismdrake::notifyd
