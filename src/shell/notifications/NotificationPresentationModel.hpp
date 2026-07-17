#pragma once

#include "Generation.hpp"
#include "NotificationModel.hpp"
#include "Result.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <memory>
#include <vector>

namespace prismdrake::shell::notifications {

/// One passive action affordance bound to the exact synthetic card content that created it.
///
/// The action identifier, notification identifier, and content generation never become QML
/// numeric properties. requestActivation() emits intent only; it performs no action itself.
class NotificationActionPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(bool enabled READ enabled CONSTANT)

  public:
    [[nodiscard]] const QString &label() const noexcept { return label_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    Q_INVOKABLE bool requestActivation();

  signals:
    void activationIntent(prismdrake::notifications::NotificationId notificationId,
                          prismdrake::foundation::Generation contentGeneration,
                          const QString &actionId);

  private:
    NotificationActionPresentation(
        prismdrake::notifications::NotificationId notificationId,
        prismdrake::foundation::Generation contentGeneration,
        const prismdrake::notifications::SyntheticNotificationAction &action, QObject *parent);

    prismdrake::notifications::NotificationId notification_id_;
    prismdrake::foundation::Generation content_generation_;
    QString action_id_;
    QString label_;
    bool enabled_;

    friend class NotificationActionPresentationModel;
};

/// Bounded action rows owned by one immutable card presentation.
class NotificationActionPresentationModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int {
        actionObject = Qt::UserRole + 1,
        label,
        enabled,
    };
    Q_ENUM(Role)

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] NotificationActionPresentation *actionAt(int row) const noexcept;

  signals:
    void activationIntent(prismdrake::notifications::NotificationId notificationId,
                          prismdrake::foundation::Generation contentGeneration,
                          const QString &actionId);

  private:
    NotificationActionPresentationModel(
        prismdrake::notifications::NotificationId notificationId,
        prismdrake::foundation::Generation contentGeneration,
        const std::vector<prismdrake::notifications::SyntheticNotificationAction> &actions,
        QObject *parent);

    std::vector<std::unique_ptr<NotificationActionPresentation>> actions_;

    friend class NotificationCardPresentation;
};

/// One immutable card DTO. Text is exposed exactly as literal Unicode data.
class NotificationCardPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString summary READ summary CONSTANT)
    Q_PROPERTY(QString body READ body CONSTANT)
    Q_PROPERTY(QString applicationName READ applicationName CONSTANT)
    Q_PROPERTY(QString urgencyId READ urgencyId CONSTANT)
    Q_PROPERTY(bool dismissible READ dismissible CONSTANT)
    Q_PROPERTY(QAbstractListModel *actions READ actions CONSTANT)

  public:
    [[nodiscard]] const QString &summary() const noexcept { return summary_; }
    [[nodiscard]] const QString &body() const noexcept { return body_; }
    [[nodiscard]] const QString &applicationName() const noexcept { return application_name_; }
    [[nodiscard]] const QString &urgencyId() const noexcept { return urgency_id_; }
    [[nodiscard]] bool dismissible() const noexcept { return dismissible_; }
    [[nodiscard]] QAbstractListModel *actions() const noexcept { return actions_.get(); }

    Q_INVOKABLE bool requestDismissal();

  signals:
    void actionIntent(prismdrake::notifications::NotificationId notificationId,
                      prismdrake::foundation::Generation contentGeneration,
                      const QString &actionId);
    void dismissIntent(prismdrake::notifications::NotificationId notificationId,
                       prismdrake::foundation::Generation contentGeneration);

  private:
    explicit NotificationCardPresentation(const prismdrake::notifications::NotificationCard &card,
                                          QObject *parent);

    prismdrake::notifications::NotificationId notification_id_;
    prismdrake::foundation::Generation content_generation_;
    QString summary_;
    QString body_;
    QString application_name_;
    QString urgency_id_;
    bool dismissible_;
    std::unique_ptr<NotificationActionPresentationModel> actions_;

    friend class NotificationPresentationModel;
};

/// Passive Qt mirror of one complete immutable synthetic-notification publication.
///
/// Applying a snapshot does not mutate the synthetic model or execute notification actions. QML
/// receives only bounded presentation properties and QObject affordances. Exact uint64 identities
/// remain inside C++ and are emitted only through the typed intent signals below.
class NotificationPresentationModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int {
        cardObject = Qt::UserRole + 1,
    };
    Q_ENUM(Role)

    explicit NotificationPresentationModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Reconciles validated rows with incremental Qt model signals.
    ///
    /// Unchanged card QObjects retain identity. During reconciliation isApplyingSnapshot() is true
    /// and currentSnapshot() still returns the prior complete publication. publicationApplied() is
    /// emitted only after the rows and current snapshot are coherent. Reentrant application fails.
    /// A rejected or stale snapshot leaves the previous rows and snapshot unchanged.
    /// This function must run on the model's owning QObject thread; cross-thread calls fail.
    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot);

    [[nodiscard]] NotificationCardPresentation *cardAt(int row) const noexcept;
    [[nodiscard]] bool isApplyingSnapshot() const noexcept { return applying_snapshot_; }
    [[nodiscard]] std::shared_ptr<const prismdrake::notifications::NotificationSnapshot>
    currentSnapshot() const noexcept {
        return snapshot_;
    }

  signals:
    void publicationReconciliationStarted();
    void publicationApplied();
    void actionRequested(prismdrake::notifications::NotificationId notificationId,
                         prismdrake::foundation::Generation contentGeneration,
                         const QString &actionId);
    void dismissRequested(prismdrake::notifications::NotificationId notificationId,
                          prismdrake::foundation::Generation contentGeneration);

  private:
    std::shared_ptr<const prismdrake::notifications::NotificationSnapshot> snapshot_;
    std::vector<std::unique_ptr<NotificationCardPresentation>> cards_;
    bool applying_snapshot_{false};
};

} // namespace prismdrake::shell::notifications

Q_DECLARE_METATYPE(prismdrake::notifications::NotificationId)
Q_DECLARE_METATYPE(prismdrake::foundation::Generation)
