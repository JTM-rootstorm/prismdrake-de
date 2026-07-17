#pragma once

#include "Result.hpp"
#include "TaskModel.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>

#include <memory>
#include <vector>

namespace prismdrake::shell::tasks {

class TaskPresentationModel;

/// One immutable, bounded task presentation tied to a WM-observed task lifetime.
///
/// X11 identifiers, lifetime identifiers, and 64-bit model generations remain private. Invokable
/// requests carry only the private lifetime to the owning model, which adds its current coherent
/// snapshot generation before forwarding a typed controller intent.
class TaskPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString applicationId READ applicationId CONSTANT)
    Q_PROPERTY(QString fallbackIconName READ fallbackIconName CONSTANT)
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool minimized READ minimized CONSTANT)
    Q_PROPERTY(bool urgent READ urgent CONSTANT)
    Q_PROPERTY(bool modal READ modal CONSTANT)
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

  public:
    [[nodiscard]] const QString &title() const noexcept { return title_; }
    [[nodiscard]] const QString &applicationId() const noexcept { return application_id_; }
    [[nodiscard]] const QString &fallbackIconName() const noexcept { return fallback_icon_name_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool minimized() const noexcept { return minimized_; }
    [[nodiscard]] bool urgent() const noexcept { return urgent_; }
    [[nodiscard]] bool modal() const noexcept { return modal_; }
    [[nodiscard]] const QString &statusText() const noexcept { return status_text_; }

    Q_INVOKABLE bool requestActivation();
    Q_INVOKABLE bool requestMinimization();
    Q_INVOKABLE bool requestClose();

  signals:
    void activationIntent(prismdrake::x11::TaskLifetimeId lifetime);
    void minimizationIntent(prismdrake::x11::TaskLifetimeId lifetime);
    void closeIntent(prismdrake::x11::TaskLifetimeId lifetime);

  private:
    TaskPresentation(const prismdrake::x11::TaskRecord &record, TaskPresentationModel *owner);

    [[nodiscard]] bool matches(const prismdrake::x11::TaskRecord &record) const;

    const prismdrake::x11::TaskLifetimeId lifetime_;
    TaskPresentationModel *const owner_;
    const QString title_;
    const QString application_id_;
    const QString fallback_icon_name_;
    const bool active_;
    const bool minimized_;
    const bool urgent_;
    const bool modal_;
    const QString status_text_;

    friend class TaskPresentationModel;
};

/// Passive Qt mirror of one complete immutable authoritative task-model publication.
///
/// The adapter performs no X11 I/O and owns no window policy. It reconciles rows by private
/// TaskLifetimeId, keeps unchanged QObject presentations stable, and emits typed intent only.
class TaskPresentationModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int {
        taskObject = Qt::UserRole + 1,
    };
    Q_ENUM(Role)

    explicit TaskPresentationModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Reconciles one newer complete snapshot with incremental Qt model signals.
    ///
    /// Changed rows are fully constructed before publicationReconciliationStarted is emitted.
    /// Rejected input and reentrant application retain the prior rows and snapshot coherently.
    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const prismdrake::x11::TaskModelSnapshot> snapshot);

    [[nodiscard]] TaskPresentation *taskAt(int row) const noexcept;
    [[nodiscard]] bool isApplyingSnapshot() const noexcept { return applying_snapshot_; }
    [[nodiscard]] std::shared_ptr<const prismdrake::x11::TaskModelSnapshot>
    currentSnapshot() const noexcept {
        return snapshot_;
    }

  signals:
    void publicationReconciliationStarted();
    void publicationApplied();
    void activationRequested(prismdrake::x11::TaskLifetimeId lifetime,
                             prismdrake::x11::TaskModelGeneration originatingGeneration);
    void minimizationRequested(prismdrake::x11::TaskLifetimeId lifetime,
                               prismdrake::x11::TaskModelGeneration originatingGeneration);
    void closeRequested(prismdrake::x11::TaskLifetimeId lifetime,
                        prismdrake::x11::TaskModelGeneration originatingGeneration);

  private:
    friend class TaskPresentation;

    [[nodiscard]] bool canForwardIntent(prismdrake::x11::TaskLifetimeId lifetime) const noexcept;
    void forwardActivation(prismdrake::x11::TaskLifetimeId lifetime);
    void forwardMinimization(prismdrake::x11::TaskLifetimeId lifetime);
    void forwardClose(prismdrake::x11::TaskLifetimeId lifetime);

    std::shared_ptr<const prismdrake::x11::TaskModelSnapshot> snapshot_;
    std::vector<std::unique_ptr<TaskPresentation>> tasks_;
    bool applying_snapshot_{false};
};

} // namespace prismdrake::shell::tasks

Q_DECLARE_METATYPE(prismdrake::x11::TaskLifetimeId)
Q_DECLARE_METATYPE(prismdrake::x11::TaskModelGeneration)
