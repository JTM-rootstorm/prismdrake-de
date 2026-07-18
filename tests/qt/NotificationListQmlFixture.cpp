#include "NotificationListQmlFixture.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <chrono>
#include <string>
#include <utility>

namespace prismdrake::shell::notifications::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] prismdrake::notifications::SyntheticNotificationInput
input(const QString &summary, int timeoutMilliseconds,
      prismdrake::notifications::NotificationUrgency urgency =
          prismdrake::notifications::NotificationUrgency::normal) {
    prismdrake::notifications::SyntheticNotificationInput value;
    const QByteArray encoded = summary.toUtf8();
    value.summary = std::string{encoded.constData(), static_cast<std::size_t>(encoded.size())};
    value.body = "Literal fixture body";
    value.applicationName = "Fixture App";
    value.actions = {{"open", "Open", true}};
    value.urgency = urgency;
    if (timeoutMilliseconds < 0) {
        value.timeout = {prismdrake::notifications::NotificationTimeoutKind::never, 0ms};
    } else {
        value.timeout = {prismdrake::notifications::NotificationTimeoutKind::explicitTimeout,
                         std::chrono::milliseconds{timeoutMilliseconds}};
    }
    return value;
}

} // namespace

NotificationListQmlFixture::NotificationListQmlFixture(QObject *parent) : QObject(parent) {
    reset();
}

QAbstractItemModel *NotificationListQmlFixture::presentationModel() const noexcept {
    return presentation_.get();
}

bool NotificationListQmlFixture::reset() {
    clock_ = std::make_shared<foundation::TestMonotonicClock>();
    auto created = prismdrake::notifications::SyntheticNotificationModel::create(clock_);
    if (!created) {
        return false;
    }
    synthetic_.emplace(std::move(created).value());
    presentation_ = std::make_unique<NotificationPresentationModel>();
    ids_.clear();
    emit presentationModelChanged();
    return true;
}

bool NotificationListQmlFixture::addCard(const QString &summary, int timeoutMilliseconds) {
    if (!synthetic_) {
        return false;
    }
    auto outcome = synthetic_->upsert(input(summary, timeoutMilliseconds));
    if (!outcome) {
        return false;
    }
    ids_.push_back(outcome.value().id);
    return apply(outcome.value().snapshot);
}

bool NotificationListQmlFixture::addCriticalCard(const QString &summary) {
    if (!synthetic_) {
        return false;
    }
    auto outcome = synthetic_->upsert(
        input(summary, -1, prismdrake::notifications::NotificationUrgency::critical));
    if (!outcome) {
        return false;
    }
    ids_.push_back(outcome.value().id);
    return apply(outcome.value().snapshot);
}

bool NotificationListQmlFixture::replaceCard(int row, const QString &summary) {
    if (!synthetic_ || row < 0 || static_cast<std::size_t>(row) >= ids_.size()) {
        return false;
    }
    auto replacement = input(summary, -1);
    replacement.replacementId = ids_[static_cast<std::size_t>(row)];
    auto outcome = synthetic_->upsert(std::move(replacement));
    return outcome && apply(outcome.value().snapshot);
}

bool NotificationListQmlFixture::dismissCard(int row) {
    if (!synthetic_ || row < 0 || static_cast<std::size_t>(row) >= ids_.size()) {
        return false;
    }
    const auto snapshot = presentation_->currentSnapshot();
    if (!snapshot || static_cast<std::size_t>(row) >= snapshot->cards.size()) {
        return false;
    }
    auto outcome =
        synthetic_->dismiss(ids_[static_cast<std::size_t>(row)],
                            snapshot->cards[static_cast<std::size_t>(row)].contentGeneration);
    if (!outcome || !apply(outcome.value().snapshot)) {
        return false;
    }
    refreshIds();
    return true;
}

bool NotificationListQmlFixture::advanceTimeouts(int milliseconds) {
    if (!synthetic_ || milliseconds < 0 ||
        !clock_->advance(std::chrono::milliseconds{milliseconds})) {
        return false;
    }
    auto outcome = synthetic_->advanceTimeouts();
    if (!outcome || !apply(outcome.value().snapshot)) {
        return false;
    }
    refreshIds();
    return true;
}

bool NotificationListQmlFixture::apply(
    std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot) {
    return presentation_ && presentation_->applySnapshot(std::move(snapshot)).hasValue();
}

void NotificationListQmlFixture::refreshIds() {
    ids_.clear();
    const auto snapshot = presentation_->currentSnapshot();
    if (!snapshot) {
        return;
    }
    ids_.reserve(snapshot->cards.size());
    for (const auto &card : snapshot->cards) {
        ids_.push_back(card.id);
    }
}

void NotificationListQmlSetup::qmlEngineAvailable(QQmlEngine *engine) {
    engine->rootContext()->setContextProperty(QStringLiteral("realNotificationFixture"),
                                              new NotificationListQmlFixture(engine));
}

} // namespace prismdrake::shell::notifications::test
