#pragma once

#include "NotificationModel.hpp"
#include "NotificationPresentationModel.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QQmlEngine;

namespace prismdrake::shell::notifications::test {

class NotificationListQmlFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *presentationModel READ presentationModel NOTIFY
                   presentationModelChanged)

  public:
    explicit NotificationListQmlFixture(QObject *parent = nullptr);

    [[nodiscard]] QAbstractItemModel *presentationModel() const noexcept;

    Q_INVOKABLE bool reset();
    Q_INVOKABLE bool addCard(const QString &summary, int timeoutMilliseconds = -1);
    Q_INVOKABLE bool replaceCard(int row, const QString &summary);
    Q_INVOKABLE bool dismissCard(int row);
    Q_INVOKABLE bool advanceTimeouts(int milliseconds);

  signals:
    void presentationModelChanged();

  private:
    [[nodiscard]] bool
    apply(std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot);
    void refreshIds();

    std::shared_ptr<foundation::TestMonotonicClock> clock_;
    std::optional<prismdrake::notifications::SyntheticNotificationModel> synthetic_;
    std::unique_ptr<NotificationPresentationModel> presentation_;
    std::vector<prismdrake::notifications::NotificationId> ids_;
};

class NotificationListQmlSetup final : public QObject {
    Q_OBJECT

  public slots:
    void qmlEngineAvailable(QQmlEngine *engine);
};

} // namespace prismdrake::shell::notifications::test
