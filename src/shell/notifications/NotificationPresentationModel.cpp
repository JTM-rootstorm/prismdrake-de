#include "NotificationPresentationModel.hpp"

#include <QModelIndex>
#include <QThread>

#include <algorithm>
#include <utility>

namespace prismdrake::shell::notifications {
namespace {

using prismdrake::foundation::Error;
using prismdrake::foundation::ErrorCode;
using prismdrake::notifications::NotificationCard;
using prismdrake::notifications::NotificationSnapshot;
using prismdrake::notifications::NotificationUrgency;
using prismdrake::notifications::SyntheticNotificationAction;

class ApplyingSnapshotGuard final {
  public:
    explicit ApplyingSnapshotGuard(bool &applying) noexcept : applying_(&applying) {
        applying = true;
    }
    ~ApplyingSnapshotGuard() {
        if (applying_ != nullptr) {
            *applying_ = false;
        }
    }

    ApplyingSnapshotGuard(const ApplyingSnapshotGuard &) = delete;
    ApplyingSnapshotGuard &operator=(const ApplyingSnapshotGuard &) = delete;

    void finish() noexcept {
        *applying_ = false;
        applying_ = nullptr;
    }

  private:
    bool *applying_;
};

[[nodiscard]] QString utf8(const std::string &text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString notificationUrgencyId(NotificationUrgency urgency) {
    switch (urgency) {
    case NotificationUrgency::low:
        return QStringLiteral("low");
    case NotificationUrgency::normal:
        return QStringLiteral("normal");
    case NotificationUrgency::critical:
        return QStringLiteral("critical");
    }
    return {};
}

} // namespace

NotificationActionPresentation::NotificationActionPresentation(
    prismdrake::notifications::NotificationId notificationId,
    prismdrake::foundation::Generation contentGeneration, const SyntheticNotificationAction &action,
    QObject *parent)
    : QObject(parent), notification_id_(notificationId), content_generation_(contentGeneration),
      action_id_(utf8(action.id)), label_(utf8(action.label)), enabled_(action.enabled) {}

bool NotificationActionPresentation::requestActivation() {
    if (!enabled_) {
        return false;
    }
    emit activationIntent(notification_id_, content_generation_, action_id_);
    return true;
}

NotificationActionPresentationModel::NotificationActionPresentationModel(
    prismdrake::notifications::NotificationId notificationId,
    prismdrake::foundation::Generation contentGeneration,
    const std::vector<SyntheticNotificationAction> &actions, QObject *parent)
    : QAbstractListModel(parent) {
    actions_.reserve(actions.size());
    for (const auto &action : actions) {
        auto presentation = std::unique_ptr<NotificationActionPresentation>(
            new NotificationActionPresentation(notificationId, contentGeneration, action, this));
        connect(presentation.get(), &NotificationActionPresentation::activationIntent, this,
                &NotificationActionPresentationModel::activationIntent);
        actions_.push_back(std::move(presentation));
    }
}

int NotificationActionPresentationModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(actions_.size());
}

QVariant NotificationActionPresentationModel::data(const QModelIndex &index, int role) const {
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

QHash<int, QByteArray> NotificationActionPresentationModel::roleNames() const {
    return {{Role::actionObject, QByteArrayLiteral("action")},
            {Role::label, QByteArrayLiteral("label")},
            {Role::enabled, QByteArrayLiteral("enabled")}};
}

NotificationActionPresentation *
NotificationActionPresentationModel::actionAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= actions_.size()) {
        return nullptr;
    }
    return actions_[static_cast<std::size_t>(row)].get();
}

NotificationCardPresentation::NotificationCardPresentation(const NotificationCard &card,
                                                           QObject *parent)
    : QObject(parent), notification_id_(card.id), content_generation_(card.contentGeneration),
      summary_(utf8(card.summary)), body_(utf8(card.body)),
      application_name_(card.applicationName ? utf8(*card.applicationName) : QString{}),
      urgency_id_(notificationUrgencyId(card.urgency)), dismissible_(card.dismissible),
      actions_(std::unique_ptr<NotificationActionPresentationModel>(
          new NotificationActionPresentationModel(card.id, card.contentGeneration, card.actions,
                                                  this))) {
    connect(actions_.get(), &NotificationActionPresentationModel::activationIntent, this,
            &NotificationCardPresentation::actionIntent);
}

bool NotificationCardPresentation::requestDismissal() {
    if (!dismissible_) {
        return false;
    }
    emit dismissIntent(notification_id_, content_generation_);
    return true;
}

NotificationPresentationModel::NotificationPresentationModel(QObject *parent)
    : QAbstractListModel(parent) {
    cards_.reserve(prismdrake::notifications::maximumNotificationCards);
}

int NotificationPresentationModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(cards_.size());
}

QVariant NotificationPresentationModel::data(const QModelIndex &index, int role) const {
    auto *card = cardAt(index.row());
    if (card == nullptr || index.parent().isValid() || role != Role::cardObject) {
        return {};
    }
    return QVariant::fromValue(static_cast<QObject *>(card));
}

QHash<int, QByteArray> NotificationPresentationModel::roleNames() const {
    return {{Role::cardObject, QByteArrayLiteral("card")}};
}

foundation::Result<void>
NotificationPresentationModel::applySnapshot(std::shared_ptr<const NotificationSnapshot> snapshot) {
    if (QThread::currentThread() != thread()) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::cancelled,
                  "synthetic notification presentation was called from a non-owner thread",
                  "queue the complete snapshot to the presentation model's QObject thread"});
    }
    if (applying_snapshot_) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::cancelled,
                  "synthetic notification presentation application is already in progress",
                  "queue the newer complete snapshot until publicationApplied is emitted"});
    }
    if (!snapshot) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::invalid_argument, "synthetic notification snapshot is absent",
                  "retain the prior presentation until a complete snapshot is available"});
    }

    auto valid = prismdrake::notifications::validateNotificationSnapshot(*snapshot);
    if (!valid) {
        return valid;
    }

    if (snapshot_) {
        if (snapshot->generation < snapshot_->generation) {
            return foundation::Result<void>::failure(
                Error{ErrorCode::cancelled, "synthetic notification snapshot is stale",
                      "retain the newer complete presentation snapshot"});
        }
        if (snapshot->generation == snapshot_->generation) {
            if (snapshot == snapshot_) {
                return foundation::Result<void>::success();
            }
            return foundation::Result<void>::failure(
                Error{ErrorCode::validation_error,
                      "synthetic notification generation has conflicting content",
                      "retain the prior presentation and publish a new generation"});
        }
    }

    ApplyingSnapshotGuard applyingGuard{applying_snapshot_};
    std::vector<std::unique_ptr<NotificationCardPresentation>> replacements(snapshot->cards.size());
    for (std::size_t index = 0U; index < snapshot->cards.size(); ++index) {
        const auto &nextCard = snapshot->cards[index];
        const auto prior = std::ranges::find_if(cards_, [&nextCard](const auto &card) {
            return card->notification_id_ == nextCard.id;
        });
        if (prior != cards_.end()) {
            const auto *priorCard = prior->get();
            if (nextCard.contentGeneration < priorCard->content_generation_) {
                return foundation::Result<void>::failure(Error{
                    ErrorCode::cancelled, "synthetic notification card content generation is stale",
                    "retain the newer complete presentation snapshot"});
            }
            if (nextCard.contentGeneration == priorCard->content_generation_) {
                const auto priorSnapshotCard = std::ranges::find_if(
                    snapshot_->cards,
                    [&nextCard](const NotificationCard &card) { return card.id == nextCard.id; });
                if (priorSnapshotCard == snapshot_->cards.end() || *priorSnapshotCard != nextCard) {
                    return foundation::Result<void>::failure(
                        Error{ErrorCode::validation_error,
                              "synthetic notification content changed without a new generation",
                              "publish changed card content with a new content generation"});
                }
                continue;
            }
        }

        replacements[index] = std::unique_ptr<NotificationCardPresentation>(
            new NotificationCardPresentation(nextCard, this));
        connect(replacements[index].get(), &NotificationCardPresentation::actionIntent, this,
                &NotificationPresentationModel::actionRequested);
        connect(replacements[index].get(), &NotificationCardPresentation::dismissIntent, this,
                &NotificationPresentationModel::dismissRequested);
    }

    emit publicationReconciliationStarted();
    for (std::size_t index = cards_.size(); index > 0U; --index) {
        const auto row = index - 1U;
        const auto retained = std::ranges::any_of(snapshot->cards, [this, row](const auto &card) {
            return card.id == cards_[row]->notification_id_;
        });
        if (!retained) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(row));
            endRemoveRows();
        }
    }

    for (std::size_t index = 0U; index < snapshot->cards.size(); ++index) {
        const auto &nextCard = snapshot->cards[index];
        auto current = std::ranges::find_if(cards_, [&nextCard](const auto &card) {
            return card->notification_id_ == nextCard.id;
        });
        if (current == cards_.end()) {
            beginInsertRows({}, static_cast<int>(index), static_cast<int>(index));
            cards_.insert(cards_.begin() + static_cast<std::ptrdiff_t>(index),
                          std::move(replacements[index]));
            endInsertRows();
            continue;
        }

        auto currentIndex = static_cast<std::size_t>(std::distance(cards_.begin(), current));
        if (currentIndex != index) {
            beginMoveRows({}, static_cast<int>(currentIndex), static_cast<int>(currentIndex), {},
                          static_cast<int>(index));
            auto moved = std::move(cards_[currentIndex]);
            cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(currentIndex));
            cards_.insert(cards_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
            endMoveRows();
            currentIndex = index;
        }
        if (replacements[index]) {
            cards_[currentIndex] = std::move(replacements[index]);
            emit dataChanged(this->index(static_cast<int>(currentIndex)),
                             this->index(static_cast<int>(currentIndex)), {Role::cardObject});
        }
    }

    snapshot_ = std::move(snapshot);
    applyingGuard.finish();
    emit publicationApplied();
    return foundation::Result<void>::success();
}

NotificationCardPresentation *NotificationPresentationModel::cardAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= cards_.size()) {
        return nullptr;
    }
    return cards_[static_cast<std::size_t>(row)].get();
}

} // namespace prismdrake::shell::notifications
