#include "NotificationCenterQmlFixture.hpp"

#include <QQmlContext>
#include <QQmlEngine>

#include <cstdint>
#include <memory>
#include <string>

namespace prismdrake::shell::notificationcenter::test {

NotificationCenterQmlFixture::NotificationCenterQmlFixture(QObject *parent)
    : QObject(parent), presentation_(this) {}

void NotificationCenterQmlFixture::reset() {
    presentation_.clear();
    generation_ = 0U;
}

bool NotificationCenterQmlFixture::publish(int recordCount, bool doNotDisturb) {
    if (recordCount < 0 || recordCount > 16) {
        return false;
    }
    auto snapshot = std::make_shared<NotificationCenterSnapshot>();
    snapshot->generation = ++generation_;
    snapshot->historyMode = notifications::HistoryMode::persistent;
    snapshot->effectiveDoNotDisturb = doNotDisturb;
    snapshot->runtimeDoNotDisturbOverride = doNotDisturb;
    for (int index = 0; index < recordCount; ++index) {
        NotificationCenterRecord record;
        record.recordId = static_cast<std::uint64_t>(index + 1);
        record.contentGeneration = 1U;
        record.notificationId = static_cast<std::uint32_t>(index + 1);
        record.applicationName = "Fixture";
        record.summary =
            index == 0 ? "<b>Literal summary</b>" : "Notification " + std::to_string(index + 1);
        record.body = "Literal body";
        record.urgency = index == 0 ? notifications::ServiceUrgency::critical
                                    : notifications::ServiceUrgency::normal;
        record.receivedAtUnixMilliseconds = static_cast<std::uint64_t>(index + 1);
        record.dismissible = true;
        record.transientPresentation = true;
        record.actions = {{"open", "Open", true}};
        snapshot->records.push_back(std::move(record));
    }
    snapshot->serializedJson = "qml-generation-" + std::to_string(generation_);
    return presentation_.applySnapshot(std::move(snapshot)).hasValue();
}

void NotificationCenterQmlSetup::qmlEngineAvailable(QQmlEngine *engine) {
    engine->rootContext()->setContextProperty(QStringLiteral("notificationCenterFixture"),
                                              new NotificationCenterQmlFixture(engine));
}

} // namespace prismdrake::shell::notificationcenter::test
