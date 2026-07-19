#pragma once

#include "NotificationCenterState.hpp"
#include "Result.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <vector>

namespace prismdrake::shell::notificationcenter {

struct NotificationRecordCommandIdentity final {
    std::uint64_t recordId{0U};
    std::uint64_t contentGeneration{0U};

    friend bool operator==(const NotificationRecordCommandIdentity &,
                           const NotificationRecordCommandIdentity &) = default;
};

class NotificationActionPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString label READ label CONSTANT)
    Q_PROPERTY(bool enabled READ enabled CONSTANT)

  public:
    [[nodiscard]] const QString &label() const noexcept { return label_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    Q_INVOKABLE bool requestActivation();

  signals:
    void activationRequested(NotificationRecordCommandIdentity identity, const QString &actionId);

  private:
    NotificationActionPresentation(std::uint64_t recordId, std::uint64_t contentGeneration,
                                   const notifications::ServiceAction &action, QObject *parent);

    NotificationRecordCommandIdentity identity_;
    QString action_id_;
    QString label_;
    bool enabled_;

    friend class NotificationActionListModel;
};

class NotificationActionListModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int { actionObject = Qt::UserRole + 1, label, enabled };
    Q_ENUM(Role)

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] NotificationActionPresentation *actionAt(int row) const noexcept;

  signals:
    void activationRequested(NotificationRecordCommandIdentity identity, const QString &actionId);

  private:
    NotificationActionListModel(std::uint64_t recordId, std::uint64_t contentGeneration,
                                const std::vector<notifications::ServiceAction> &actions,
                                QObject *parent);

    std::vector<std::unique_ptr<NotificationActionPresentation>> actions_;

    friend class NotificationRecordPresentation;
};

class NotificationRecordPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString summary READ summary CONSTANT)
    Q_PROPERTY(QString body READ body CONSTANT)
    Q_PROPERTY(QString applicationName READ applicationName CONSTANT)
    Q_PROPERTY(QString urgencyId READ urgencyId CONSTANT)
    Q_PROPERTY(bool live READ live CONSTANT)
    Q_PROPERTY(bool dismissible READ dismissible CONSTANT)
    Q_PROPERTY(bool transient READ transient CONSTANT)
    Q_PROPERTY(QAbstractListModel *actions READ actions CONSTANT)

  public:
    [[nodiscard]] const QString &summary() const noexcept { return summary_; }
    [[nodiscard]] const QString &body() const noexcept { return body_; }
    [[nodiscard]] const QString &applicationName() const noexcept { return application_name_; }
    [[nodiscard]] const QString &urgencyId() const noexcept { return urgency_id_; }
    [[nodiscard]] bool live() const noexcept { return live_; }
    [[nodiscard]] bool dismissible() const noexcept { return dismissible_; }
    [[nodiscard]] bool transient() const noexcept { return transient_; }
    [[nodiscard]] QAbstractListModel *actions() const noexcept { return actions_.get(); }

    Q_INVOKABLE bool requestDismissal();

  signals:
    void actionRequested(NotificationRecordCommandIdentity identity, const QString &actionId);
    void dismissalRequested(NotificationRecordCommandIdentity identity);

  private:
    NotificationRecordPresentation(NotificationCenterRecord record, QObject *parent);

    NotificationCenterRecord source_;
    QString summary_;
    QString body_;
    QString application_name_;
    QString urgency_id_;
    bool live_;
    bool dismissible_;
    bool transient_;
    std::unique_ptr<NotificationActionListModel> actions_;

    friend class NotificationRecordListModel;
};

/// A passive mirror of either all history rows or the current transient subset.
class NotificationRecordListModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum class Filter : std::uint8_t { history, transient };
    enum Role : int { cardObject = Qt::UserRole + 1 };
    Q_ENUM(Role)

    explicit NotificationRecordListModel(Filter filter, QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] NotificationRecordPresentation *cardAt(int row) const noexcept;

    void applySnapshot(const NotificationCenterSnapshotPtr &snapshot);
    void clear();

  signals:
    void publicationReconciliationStarted();
    void publicationApplied();
    void actionRequested(NotificationRecordCommandIdentity identity, const QString &actionId);
    void dismissalRequested(NotificationRecordCommandIdentity identity);

  private:
    [[nodiscard]] std::vector<NotificationCenterRecord>
    selectedRecords(const NotificationCenterSnapshotPtr &snapshot) const;

    Filter filter_;
    std::vector<std::unique_ptr<NotificationRecordPresentation>> records_;
};

/// Presentation-only projection of the current owner-epoch state.
///
/// Mutations are emitted as typed intents. This object never calls D-Bus or
/// becomes a second notification authority.
class NotificationCenterPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractListModel *historyModel READ historyModel CONSTANT)
    Q_PROPERTY(QAbstractListModel *transientModel READ transientModel CONSTANT)
    Q_PROPERTY(bool serviceAvailable READ serviceAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool effectiveDoNotDisturb READ effectiveDoNotDisturb NOTIFY stateChanged)
    Q_PROPERTY(
        bool hasRuntimeDoNotDisturbOverride READ hasRuntimeDoNotDisturbOverride NOTIFY stateChanged)
    Q_PROPERTY(
        bool runtimeDoNotDisturbOverride READ runtimeDoNotDisturbOverride NOTIFY stateChanged)
    Q_PROPERTY(QString historyModeId READ historyModeId NOTIFY stateChanged)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY stateChanged)

  public:
    explicit NotificationCenterPresentation(QObject *parent = nullptr);

    [[nodiscard]] QAbstractListModel *historyModel() noexcept { return &history_model_; }
    [[nodiscard]] QAbstractListModel *transientModel() noexcept { return &transient_model_; }
    [[nodiscard]] bool serviceAvailable() const noexcept { return snapshot_ != nullptr; }
    [[nodiscard]] bool effectiveDoNotDisturb() const noexcept;
    [[nodiscard]] bool hasRuntimeDoNotDisturbOverride() const noexcept;
    [[nodiscard]] bool runtimeDoNotDisturbOverride() const noexcept;
    [[nodiscard]] QString historyModeId() const;
    [[nodiscard]] int historyCount() const noexcept;
    [[nodiscard]] const NotificationCenterSnapshotPtr &currentSnapshot() const noexcept {
        return snapshot_;
    }

    [[nodiscard]] foundation::Result<void> applySnapshot(NotificationCenterSnapshotPtr snapshot);
    void clear();

    Q_INVOKABLE bool requestDoNotDisturb(bool enabled);
    Q_INVOKABLE bool requestClearHistory();

  signals:
    void stateChanged();
    void doNotDisturbRequested(bool enabled);
    void clearHistoryRequested();
    void actionRequested(NotificationRecordCommandIdentity identity, const QString &actionId);
    void dismissalRequested(NotificationRecordCommandIdentity identity);

  private:
    NotificationCenterSnapshotPtr snapshot_;
    NotificationRecordListModel history_model_;
    NotificationRecordListModel transient_model_;
};

} // namespace prismdrake::shell::notificationcenter

Q_DECLARE_METATYPE(prismdrake::shell::notificationcenter::NotificationRecordCommandIdentity)
