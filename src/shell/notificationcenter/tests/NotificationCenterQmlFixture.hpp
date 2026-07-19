#pragma once

#include "NotificationCenterPresentation.hpp"

#include <QObject>

class QQmlEngine;

namespace prismdrake::shell::notificationcenter::test {

class NotificationCenterQmlFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *presentation READ presentation CONSTANT)

  public:
    explicit NotificationCenterQmlFixture(QObject *parent = nullptr);

    [[nodiscard]] QObject *presentation() noexcept { return &presentation_; }

    Q_INVOKABLE void reset();
    Q_INVOKABLE bool publish(int recordCount, bool doNotDisturb = false);

  private:
    NotificationCenterPresentation presentation_;
    std::uint64_t generation_{0U};
};

class NotificationCenterQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::notificationcenter::test
