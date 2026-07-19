#include "NotificationCenterPresentation.hpp"

#include <QModelIndex>
#include <QThread>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <utility>

namespace prismdrake::shell::notificationcenter {
namespace {

[[nodiscard]] QString utf8(const std::string &text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString urgencyTextId(notifications::ServiceUrgency urgency) {
    switch (urgency) {
    case notifications::ServiceUrgency::low:
        return QStringLiteral("low");
    case notifications::ServiceUrgency::normal:
        return QStringLiteral("normal");
    case notifications::ServiceUrgency::critical:
        return QStringLiteral("critical");
    }
    return {};
}

[[nodiscard]] foundation::Error invalidSnapshot() {
    return {foundation::ErrorCode::invalid_argument,
            "The notification presentation snapshot is invalid.",
            "Publish one complete current owner-epoch snapshot."};
}

} // namespace

NotificationActionPresentation::NotificationActionPresentation(
    std::uint64_t recordId, std::uint64_t contentGeneration,
    const notifications::ServiceAction &action, QObject *parent)
    : QObject(parent), identity_{recordId, contentGeneration}, action_id_(utf8(action.id)),
      label_(utf8(action.label)), enabled_(action.enabled) {}

bool NotificationActionPresentation::requestActivation() {
    if (!enabled_) {
        return false;
    }
    emit activationRequested(identity_, action_id_);
    return true;
}

NotificationActionListModel::NotificationActionListModel(
    std::uint64_t recordId, std::uint64_t contentGeneration,
    const std::vector<notifications::ServiceAction> &actions, QObject *parent)
    : QAbstractListModel(parent) {
    actions_.reserve(actions.size());
    for (const auto &action : actions) {
        auto presentation = std::unique_ptr<NotificationActionPresentation>(
            new NotificationActionPresentation(recordId, contentGeneration, action, this));
        connect(presentation.get(), &NotificationActionPresentation::activationRequested, this,
                &NotificationActionListModel::activationRequested);
        actions_.push_back(std::move(presentation));
    }
}

int NotificationActionListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(actions_.size());
}

QVariant NotificationActionListModel::data(const QModelIndex &index, int role) const {
    const auto *action = actionAt(index.row());
    if (action == nullptr || index.parent().isValid()) {
        return {};
    }
    switch (role) {
    case Role::actionObject:
        return QVariant::fromValue(
            static_cast<QObject *>(const_cast<NotificationActionPresentation *>(action)));
    case Role::label:
        return action->label();
    case Role::enabled:
        return action->enabled();
    default:
        return {};
    }
}

QHash<int, QByteArray> NotificationActionListModel::roleNames() const {
    return {{Role::actionObject, QByteArrayLiteral("action")},
            {Role::label, QByteArrayLiteral("label")},
            {Role::enabled, QByteArrayLiteral("enabled")}};
}

NotificationActionPresentation *NotificationActionListModel::actionAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= actions_.size()) {
        return nullptr;
    }
    return actions_[static_cast<std::size_t>(row)].get();
}

NotificationRecordPresentation::NotificationRecordPresentation(NotificationCenterRecord record,
                                                               QObject *parent)
    : QObject(parent), source_(std::move(record)), summary_(utf8(source_.summary)),
      body_(utf8(source_.body)),
      application_name_(source_.applicationName ? utf8(*source_.applicationName) : QString{}),
      urgency_id_(urgencyTextId(source_.urgency)), live_(source_.notificationId.has_value()),
      dismissible_(source_.dismissible && live_), transient_(source_.transientPresentation),
      actions_(std::unique_ptr<NotificationActionListModel>(new NotificationActionListModel(
          source_.recordId, source_.contentGeneration, source_.actions, this))) {
    connect(actions_.get(), &NotificationActionListModel::activationRequested, this,
            &NotificationRecordPresentation::actionRequested);
}

bool NotificationRecordPresentation::requestDismissal() {
    if (!dismissible_) {
        return false;
    }
    emit dismissalRequested({source_.recordId, source_.contentGeneration});
    return true;
}

NotificationRecordListModel::NotificationRecordListModel(Filter filter, QObject *parent)
    : QAbstractListModel(parent), filter_(filter) {}

int NotificationRecordListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(records_.size());
}

QVariant NotificationRecordListModel::data(const QModelIndex &index, int role) const {
    auto *card = cardAt(index.row());
    if (card == nullptr || index.parent().isValid() || role != Role::cardObject) {
        return {};
    }
    return QVariant::fromValue(static_cast<QObject *>(card));
}

QHash<int, QByteArray> NotificationRecordListModel::roleNames() const {
    return {{Role::cardObject, QByteArrayLiteral("card")}};
}

NotificationRecordPresentation *NotificationRecordListModel::cardAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= records_.size()) {
        return nullptr;
    }
    return records_[static_cast<std::size_t>(row)].get();
}

std::vector<NotificationCenterRecord>
NotificationRecordListModel::selectedRecords(const NotificationCenterSnapshotPtr &snapshot) const {
    std::vector<NotificationCenterRecord> selected;
    if (!snapshot) {
        return selected;
    }
    selected.reserve(snapshot->records.size());
    for (const auto &record : snapshot->records) {
        if (filter_ == Filter::history || record.transientPresentation) {
            selected.push_back(record);
        }
    }
    return selected;
}

void NotificationRecordListModel::applySnapshot(const NotificationCenterSnapshotPtr &snapshot) {
    const auto selected = selectedRecords(snapshot);
    emit publicationReconciliationStarted();

    std::vector<std::unique_ptr<NotificationRecordPresentation>> replacements;
    replacements.reserve(selected.size());
    for (const auto &record : selected) {
        const auto current = std::ranges::find_if(records_, [&record](const auto &candidate) {
            return candidate->source_.recordId == record.recordId;
        });
        if (current != records_.end() && (*current)->source_ == record) {
            replacements.push_back(nullptr);
        } else {
            auto replacement = std::unique_ptr<NotificationRecordPresentation>(
                new NotificationRecordPresentation(record, this));
            connect(replacement.get(), &NotificationRecordPresentation::actionRequested, this,
                    &NotificationRecordListModel::actionRequested);
            connect(replacement.get(), &NotificationRecordPresentation::dismissalRequested, this,
                    &NotificationRecordListModel::dismissalRequested);
            replacements.push_back(std::move(replacement));
        }
    }

    for (std::size_t index = records_.size(); index > 0U; --index) {
        const auto row = index - 1U;
        if (!std::ranges::any_of(selected, [this, row](const auto &record) {
                return record.recordId == records_[row]->source_.recordId;
            })) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(row));
            endRemoveRows();
        }
    }

    for (std::size_t index = 0U; index < selected.size(); ++index) {
        const auto &record = selected[index];
        auto current = std::ranges::find_if(records_, [&record](const auto &candidate) {
            return candidate->source_.recordId == record.recordId;
        });
        if (current == records_.end()) {
            beginInsertRows({}, static_cast<int>(index), static_cast<int>(index));
            records_.insert(records_.begin() + static_cast<std::ptrdiff_t>(index),
                            std::move(replacements[index]));
            endInsertRows();
            continue;
        }

        auto currentIndex = static_cast<std::size_t>(std::distance(records_.begin(), current));
        if (currentIndex != index) {
            beginMoveRows({}, static_cast<int>(currentIndex), static_cast<int>(currentIndex), {},
                          static_cast<int>(index));
            auto moved = std::move(records_[currentIndex]);
            records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(currentIndex));
            records_.insert(records_.begin() + static_cast<std::ptrdiff_t>(index),
                            std::move(moved));
            endMoveRows();
            currentIndex = index;
        }
        if (replacements[index]) {
            records_[currentIndex] = std::move(replacements[index]);
            emit dataChanged(this->index(static_cast<int>(currentIndex)),
                             this->index(static_cast<int>(currentIndex)), {Role::cardObject});
        }
    }
    emit publicationApplied();
}

void NotificationRecordListModel::clear() {
    if (records_.empty()) {
        return;
    }
    emit publicationReconciliationStarted();
    beginResetModel();
    records_.clear();
    endResetModel();
    emit publicationApplied();
}

NotificationCenterPresentation::NotificationCenterPresentation(QObject *parent)
    : QObject(parent), history_model_(NotificationRecordListModel::Filter::history, this),
      transient_model_(NotificationRecordListModel::Filter::transient, this) {
    for (auto *model : {&history_model_, &transient_model_}) {
        connect(model, &NotificationRecordListModel::actionRequested, this,
                &NotificationCenterPresentation::actionRequested);
        connect(model, &NotificationRecordListModel::dismissalRequested, this,
                &NotificationCenterPresentation::dismissalRequested);
    }
}

bool NotificationCenterPresentation::effectiveDoNotDisturb() const noexcept {
    return snapshot_ && snapshot_->effectiveDoNotDisturb;
}

bool NotificationCenterPresentation::hasRuntimeDoNotDisturbOverride() const noexcept {
    return snapshot_ && snapshot_->runtimeDoNotDisturbOverride.has_value();
}

bool NotificationCenterPresentation::runtimeDoNotDisturbOverride() const noexcept {
    return snapshot_ && snapshot_->runtimeDoNotDisturbOverride.value_or(false);
}

QString NotificationCenterPresentation::historyModeId() const {
    if (!snapshot_) {
        return QStringLiteral("unavailable");
    }
    switch (snapshot_->historyMode) {
    case notifications::HistoryMode::disabled:
        return QStringLiteral("disabled");
    case notifications::HistoryMode::session:
        return QStringLiteral("session");
    case notifications::HistoryMode::persistent:
        return QStringLiteral("persistent");
    }
    return {};
}

int NotificationCenterPresentation::historyCount() const noexcept {
    return history_model_.rowCount();
}

foundation::Result<void>
NotificationCenterPresentation::applySnapshot(NotificationCenterSnapshotPtr snapshot) {
    if (thread() != QThread::currentThread() || !snapshot || snapshot->generation == 0U ||
        (snapshot_ && snapshot->generation < snapshot_->generation)) {
        return foundation::Result<void>::failure(invalidSnapshot());
    }
    if (snapshot_ && snapshot->generation == snapshot_->generation &&
        snapshot->serializedJson != snapshot_->serializedJson) {
        return foundation::Result<void>::failure(invalidSnapshot());
    }
    if (snapshot_ && snapshot->serializedJson == snapshot_->serializedJson) {
        return foundation::Result<void>::success();
    }
    history_model_.applySnapshot(snapshot);
    transient_model_.applySnapshot(snapshot);
    snapshot_ = std::move(snapshot);
    emit stateChanged();
    return foundation::Result<void>::success();
}

void NotificationCenterPresentation::clear() {
    if (!snapshot_) {
        return;
    }
    history_model_.clear();
    transient_model_.clear();
    snapshot_.reset();
    emit stateChanged();
}

bool NotificationCenterPresentation::requestDoNotDisturb(bool enabled) {
    if (!snapshot_ || snapshot_->effectiveDoNotDisturb == enabled) {
        return false;
    }
    emit doNotDisturbRequested(enabled);
    return true;
}

bool NotificationCenterPresentation::requestClearHistory() {
    if (!snapshot_ || snapshot_->records.empty() ||
        snapshot_->historyMode == notifications::HistoryMode::disabled) {
        return false;
    }
    emit clearHistoryRequested();
    return true;
}

} // namespace prismdrake::shell::notificationcenter
