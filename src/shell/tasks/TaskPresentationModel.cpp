#include "TaskPresentationModel.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QModelIndex>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace prismdrake::shell::tasks {
namespace {

using prismdrake::foundation::Error;
using prismdrake::foundation::ErrorCode;
using prismdrake::x11::TaskLifetimeId;
using prismdrake::x11::TaskModelGeneration;
using prismdrake::x11::TaskModelSnapshot;
using prismdrake::x11::TaskRecord;

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

[[nodiscard]] bool validUtf8(const std::string &text) noexcept {
    return QByteArrayView{text.data(), static_cast<qsizetype>(text.size())}.isValidUtf8();
}

[[nodiscard]] QString taskStatusText(const TaskRecord &record) {
    QStringList states;
    if (record.active()) {
        states.push_back(QCoreApplication::translate("TaskPresentation", "Active"));
    }
    if (record.hidden()) {
        states.push_back(QCoreApplication::translate("TaskPresentation", "Minimized"));
    }
    if (record.urgent()) {
        states.push_back(QCoreApplication::translate("TaskPresentation", "Urgent"));
    }
    if (record.modal()) {
        states.push_back(QCoreApplication::translate("TaskPresentation", "Modal"));
    }
    if (states.isEmpty()) {
        states.push_back(QCoreApplication::translate("TaskPresentation", "Inactive"));
    }
    return states.join(QStringLiteral(", "));
}

[[nodiscard]] foundation::Result<void> validateSnapshot(const TaskModelSnapshot &snapshot) {
    if (snapshot.tasks().size() > prismdrake::x11::maximumTaskWindows) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::too_large, "task presentation snapshot exceeds its row bound",
                  "retain the prior presentation until a bounded snapshot is available"});
    }

    std::set<TaskLifetimeId::Value> lifetimes;
    for (const auto &record : snapshot.tasks()) {
        if (record.lastObservedGeneration() != snapshot.generation() ||
            !lifetimes.insert(record.lifetime().value()).second || record.title().empty() ||
            record.title().size() > prismdrake::x11::maximumWindowTitleBytes ||
            record.applicationId().empty() ||
            record.applicationId().size() > prismdrake::x11::maximumTaskApplicationIdBytes ||
            record.fallbackIconName().empty() ||
            record.fallbackIconName().size() > prismdrake::x11::maximumTaskFallbackIconNameBytes ||
            !validUtf8(record.title()) || !validUtf8(record.applicationId()) ||
            !validUtf8(record.fallbackIconName())) {
            return foundation::Result<void>::failure(
                Error{ErrorCode::validation_error, "task presentation snapshot is malformed",
                      "retain the prior presentation and rebuild from the task model owner"});
        }
    }
    return foundation::Result<void>::success();
}

} // namespace

TaskPresentation::TaskPresentation(const TaskRecord &record, TaskPresentationModel *owner)
    : QObject(owner), lifetime_(record.lifetime()), owner_(owner), title_(utf8(record.title())),
      application_id_(utf8(record.applicationId())),
      fallback_icon_name_(utf8(record.fallbackIconName())), active_(record.active()),
      minimized_(record.hidden()), urgent_(record.urgent()), modal_(record.modal()),
      status_text_(taskStatusText(record)) {}

bool TaskPresentation::matches(const TaskRecord &record) const {
    return lifetime_ == record.lifetime() && title_ == utf8(record.title()) &&
           application_id_ == utf8(record.applicationId()) &&
           fallback_icon_name_ == utf8(record.fallbackIconName()) && active_ == record.active() &&
           minimized_ == record.hidden() && urgent_ == record.urgent() && modal_ == record.modal();
}

bool TaskPresentation::requestActivation() {
    if (!owner_->canForwardIntent(lifetime_)) {
        return false;
    }
    emit activationIntent(lifetime_);
    return true;
}

bool TaskPresentation::requestMinimization() {
    if (!owner_->canForwardIntent(lifetime_)) {
        return false;
    }
    emit minimizationIntent(lifetime_);
    return true;
}

bool TaskPresentation::requestClose() {
    if (!owner_->canForwardIntent(lifetime_)) {
        return false;
    }
    emit closeIntent(lifetime_);
    return true;
}

TaskPresentationModel::TaskPresentationModel(QObject *parent) : QAbstractListModel(parent) {
    tasks_.reserve(prismdrake::x11::maximumTaskWindows);
}

int TaskPresentationModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tasks_.size());
}

QVariant TaskPresentationModel::data(const QModelIndex &index, int role) const {
    auto *task = taskAt(index.row());
    if (task == nullptr || index.parent().isValid() || role != Role::taskObject) {
        return {};
    }
    return QVariant::fromValue(static_cast<QObject *>(task));
}

QHash<int, QByteArray> TaskPresentationModel::roleNames() const {
    return {{Role::taskObject, QByteArrayLiteral("task")}};
}

foundation::Result<void>
TaskPresentationModel::applySnapshot(std::shared_ptr<const TaskModelSnapshot> snapshot) {
    if (QThread::currentThread() != thread()) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::cancelled, "task presentation was called from a non-owner thread",
                  "queue the complete snapshot to the presentation model's QObject thread"});
    }
    if (applying_snapshot_) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::cancelled, "task presentation application is already in progress",
                  "queue the newer complete snapshot until publicationApplied is emitted"});
    }
    if (!snapshot) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::invalid_argument, "task presentation snapshot is absent",
                  "retain the prior presentation until a complete snapshot is available"});
    }

    auto valid = validateSnapshot(*snapshot);
    if (!valid) {
        return valid;
    }

    if (snapshot_) {
        if (snapshot->generation().value() < snapshot_->generation().value()) {
            return foundation::Result<void>::failure(
                Error{ErrorCode::cancelled, "task presentation snapshot is stale",
                      "retain the newer complete presentation snapshot"});
        }
        if (snapshot->generation() == snapshot_->generation()) {
            if (snapshot.get() == snapshot_.get()) {
                return foundation::Result<void>::success();
            }
            return foundation::Result<void>::failure(
                Error{ErrorCode::validation_error,
                      "task model generation has conflicting presentation content",
                      "retain the prior presentation and publish a new task generation"});
        }
    }

    std::vector<std::unique_ptr<TaskPresentation>> replacements;
    try {
        replacements.resize(snapshot->tasks().size());
        for (std::size_t index = 0U; index < snapshot->tasks().size(); ++index) {
            const auto &nextTask = snapshot->tasks()[index];
            const auto prior = std::ranges::find_if(tasks_, [&nextTask](const auto &task) {
                return task->lifetime_ == nextTask.lifetime();
            });
            if (prior != tasks_.end() && (*prior)->matches(nextTask)) {
                continue;
            }
            replacements[index] =
                std::unique_ptr<TaskPresentation>(new TaskPresentation(nextTask, this));
            connect(replacements[index].get(), &TaskPresentation::activationIntent, this,
                    &TaskPresentationModel::forwardActivation);
            connect(replacements[index].get(), &TaskPresentation::minimizationIntent, this,
                    &TaskPresentationModel::forwardMinimization);
            connect(replacements[index].get(), &TaskPresentation::closeIntent, this,
                    &TaskPresentationModel::forwardClose);
        }
    } catch (const std::exception &) {
        return foundation::Result<void>::failure(
            Error{ErrorCode::too_large, "task presentation preflight could not allocate rows",
                  "retain the prior presentation and reduce the bounded task set"});
    }

    ApplyingSnapshotGuard applyingGuard{applying_snapshot_};
    emit publicationReconciliationStarted();

    for (std::size_t index = tasks_.size(); index > 0U; --index) {
        const auto row = index - 1U;
        const auto retained =
            std::ranges::any_of(snapshot->tasks(), [this, row](const TaskRecord &record) {
                return record.lifetime() == tasks_[row]->lifetime_;
            });
        if (!retained) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(row));
            endRemoveRows();
        }
    }

    for (std::size_t index = 0U; index < snapshot->tasks().size(); ++index) {
        const auto &nextTask = snapshot->tasks()[index];
        auto current = std::ranges::find_if(tasks_, [&nextTask](const auto &task) {
            return task->lifetime_ == nextTask.lifetime();
        });
        if (current == tasks_.end()) {
            beginInsertRows({}, static_cast<int>(index), static_cast<int>(index));
            tasks_.insert(tasks_.begin() + static_cast<std::ptrdiff_t>(index),
                          std::move(replacements[index]));
            endInsertRows();
            continue;
        }

        auto currentIndex = static_cast<std::size_t>(std::distance(tasks_.begin(), current));
        if (currentIndex != index) {
            beginMoveRows({}, static_cast<int>(currentIndex), static_cast<int>(currentIndex), {},
                          static_cast<int>(index));
            auto moved = std::move(tasks_[currentIndex]);
            tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(currentIndex));
            tasks_.insert(tasks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
            endMoveRows();
            currentIndex = index;
        }
        if (replacements[index]) {
            tasks_[currentIndex] = std::move(replacements[index]);
            emit dataChanged(this->index(static_cast<int>(currentIndex)),
                             this->index(static_cast<int>(currentIndex)), {Role::taskObject});
        }
    }

    snapshot_ = std::move(snapshot);
    applyingGuard.finish();
    emit publicationApplied();
    return foundation::Result<void>::success();
}

bool TaskPresentationModel::canForwardIntent(TaskLifetimeId lifetime) const noexcept {
    if (QThread::currentThread() != thread() || applying_snapshot_ || !snapshot_) {
        return false;
    }
    const auto row = std::ranges::find_if(
        tasks_, [lifetime](const auto &task) { return task->lifetime_ == lifetime; });
    return row != tasks_.end() &&
           std::ranges::any_of(snapshot_->tasks(), [lifetime](const TaskRecord &record) {
               return record.lifetime() == lifetime;
           });
}

void TaskPresentationModel::forwardActivation(TaskLifetimeId lifetime) {
    if (canForwardIntent(lifetime)) {
        emit activationRequested(lifetime, snapshot_->generation());
    }
}

void TaskPresentationModel::forwardMinimization(TaskLifetimeId lifetime) {
    if (canForwardIntent(lifetime)) {
        emit minimizationRequested(lifetime, snapshot_->generation());
    }
}

void TaskPresentationModel::forwardClose(TaskLifetimeId lifetime) {
    if (canForwardIntent(lifetime)) {
        emit closeRequested(lifetime, snapshot_->generation());
    }
}

TaskPresentation *TaskPresentationModel::taskAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= tasks_.size()) {
        return nullptr;
    }
    return tasks_[static_cast<std::size_t>(row)].get();
}

} // namespace prismdrake::shell::tasks
