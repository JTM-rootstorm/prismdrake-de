#include "GroupedTaskPresentationModel.hpp"

#include <QCoreApplication>
#include <QModelIndex>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace prismdrake::shell::panel {
namespace {

using foundation::ErrorCode;

[[nodiscard]] QString utf8(const std::string &value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString memberStatus(const PanelTaskMember &member) {
    QStringList values;
    if (member.active) {
        values.push_back(QCoreApplication::translate("GroupedTaskMember", "Active"));
    }
    if (member.minimized) {
        values.push_back(QCoreApplication::translate("GroupedTaskMember", "Minimized"));
    }
    if (member.urgent) {
        values.push_back(QCoreApplication::translate("GroupedTaskMember", "Urgent"));
    }
    if (member.modal) {
        values.push_back(QCoreApplication::translate("GroupedTaskMember", "Modal"));
    }
    if (values.empty()) {
        values.push_back(QCoreApplication::translate("GroupedTaskMember", "Inactive"));
    }
    return values.join(QStringLiteral(", "));
}

[[nodiscard]] QString groupStatus(const PanelTaskGroup &group) {
    if (group.members.empty()) {
        return group.launchAvailable
                   ? QCoreApplication::translate("GroupedTask", "Pinned, not running")
                   : QCoreApplication::translate("GroupedTask", "Pinned application unavailable");
    }
    QStringList values;
    if (group.pinned) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Pinned"));
    }
    if (group.members.size() > 1U) {
        values.push_back(QCoreApplication::translate("GroupedTask", "%n windows", nullptr,
                                                     static_cast<int>(group.members.size())));
    }
    if (group.active) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Active"));
    }
    if (group.minimized) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Minimized"));
    }
    if (group.urgent) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Urgent"));
    }
    if (group.modal) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Modal"));
    }
    if (values.empty()) {
        values.push_back(QCoreApplication::translate("GroupedTask", "Running"));
    }
    return values.join(QStringLiteral(", "));
}

[[nodiscard]] foundation::Result<void> validateSnapshot(const GroupedTaskSnapshot &snapshot) {
    if (snapshot.taskGeneration.value() == 0U || snapshot.associationGeneration == 0U ||
        snapshot.catalogGeneration == 0U || snapshot.pinGeneration == 0U ||
        snapshot.settingsGeneration == 0U || snapshot.widthGeneration == 0U ||
        snapshot.groups.size() > x11::maximumTaskWindows + maximumPinCount) {
        return foundation::Result<void>::failure(
            {ErrorCode::validation_error, "The grouped task presentation snapshot is invalid.",
             "Retain the prior complete panel generation."});
    }
    std::set<std::string> keys;
    std::set<x11::TaskLifetimeId::Value> lifetimes;
    for (const auto &group : snapshot.groups) {
        if (group.internalKey.empty() || group.displayName.empty() || group.iconName.empty() ||
            !keys.insert(group.internalKey).second ||
            group.members.size() > x11::maximumTaskWindows ||
            (group.members.empty() && group.primaryAction == PanelTaskPrimaryAction::openGroup) ||
            (group.primaryAction == PanelTaskPrimaryAction::launch && !group.desktopFileId) ||
            (group.members.size() == 1U &&
             group.primaryAction != PanelTaskPrimaryAction::activateOrMinimize) ||
            (group.members.size() > 1U &&
             group.primaryAction != PanelTaskPrimaryAction::openGroup)) {
            return foundation::Result<void>::failure(
                {ErrorCode::validation_error, "A grouped task presentation row is invalid.",
                 "Retain the prior complete panel generation."});
        }
        for (const auto &member : group.members) {
            if (member.title.empty() || !lifetimes.insert(member.lifetime.value()).second) {
                return foundation::Result<void>::failure(
                    {ErrorCode::validation_error, "A grouped task member is invalid.",
                     "Retain the prior complete panel generation."});
            }
        }
    }
    return foundation::Result<void>::success();
}

} // namespace

GroupedTaskMemberPresentation::GroupedTaskMemberPresentation(const PanelTaskMember &member,
                                                             GroupedTaskPresentationModel *owner,
                                                             GroupedTaskPresentation *group)
    : QObject(group), lifetime_(member.lifetime), owner_(owner), group_(group),
      title_(utf8(member.title)), active_(member.active), minimized_(member.minimized),
      urgent_(member.urgent), modal_(member.modal), status_text_(memberStatus(member)) {}

bool GroupedTaskMemberPresentation::matches(const PanelTaskMember &member) const {
    return lifetime_ == member.lifetime && title_ == utf8(member.title) &&
           active_ == member.active && minimized_ == member.minimized && urgent_ == member.urgent &&
           modal_ == member.modal;
}

bool GroupedTaskMemberPresentation::requestActivation() { return owner_->requestActivation(*this); }

bool GroupedTaskMemberPresentation::requestMinimization() {
    return owner_->requestMinimization(*this);
}

bool GroupedTaskMemberPresentation::requestClose() { return owner_->requestClose(*this); }

GroupedTaskPresentation::GroupedTaskPresentation(const PanelTaskGroup &group,
                                                 GroupedTaskPresentationModel *owner)
    : QObject(owner), owner_(owner), kind_(group.kind), internal_key_(group.internalKey),
      desktop_file_id_(group.desktopFileId), display_name_(utf8(group.displayName)),
      icon_name_(utf8(group.iconName)), pinned_(group.pinned),
      launch_available_(group.launchAvailable), active_(group.active), minimized_(group.minimized),
      urgent_(group.urgent), modal_(group.modal), primary_action_(group.primaryAction),
      status_text_(groupStatus(group)) {
    members_.reserve(group.members.size());
    for (const auto &member : group.members) {
        members_.push_back(std::unique_ptr<GroupedTaskMemberPresentation>(
            new GroupedTaskMemberPresentation(member, owner, this)));
    }
}

bool GroupedTaskPresentation::matches(const PanelTaskGroup &group) const {
    return kind_ == group.kind && internal_key_ == group.internalKey &&
           desktop_file_id_ == group.desktopFileId && display_name_ == utf8(group.displayName) &&
           icon_name_ == utf8(group.iconName) && pinned_ == group.pinned &&
           launch_available_ == group.launchAvailable && active_ == group.active &&
           minimized_ == group.minimized && urgent_ == group.urgent && modal_ == group.modal &&
           primary_action_ == group.primaryAction && members_.size() == group.members.size() &&
           std::equal(members_.begin(), members_.end(), group.members.begin(),
                      [](const auto &presentation, const auto &member) {
                          return presentation->matches(member);
                      });
}

QVariantList GroupedTaskPresentation::members() const {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(members_.size()));
    for (const auto &member : members_) {
        result.push_back(QVariant::fromValue(static_cast<QObject *>(member.get())));
    }
    return result;
}

bool GroupedTaskPresentation::requestPrimaryAction() { return owner_->requestPrimary(*this); }

bool GroupedTaskPresentation::requestOpenNew() { return owner_->requestOpenNew(*this); }

bool GroupedTaskPresentation::requestPinToggle() { return owner_->requestPinToggle(*this); }

bool GroupedTaskPresentation::requestOnlyWindowClose() {
    if (members_.size() != 1U) {
        return false;
    }
    return owner_->requestClose(*members_.front());
}

GroupedTaskPresentationModel::GroupedTaskPresentationModel(QObject *parent)
    : QAbstractListModel(parent) {
    groups_.reserve(x11::maximumTaskWindows + maximumPinCount);
}

int GroupedTaskPresentationModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(groups_.size());
}

QVariant GroupedTaskPresentationModel::data(const QModelIndex &index, int role) const {
    auto *group = groupAt(index.row());
    if (group == nullptr || index.parent().isValid() || role != Role::groupObject) {
        return {};
    }
    return QVariant::fromValue(static_cast<QObject *>(group));
}

QHash<int, QByteArray> GroupedTaskPresentationModel::roleNames() const {
    return {{Role::groupObject, QByteArrayLiteral("group")}};
}

foundation::Result<void>
GroupedTaskPresentationModel::applySnapshot(std::shared_ptr<const GroupedTaskSnapshot> snapshot) {
    if (QThread::currentThread() != thread() || applying_snapshot_) {
        return foundation::Result<void>::failure(
            {ErrorCode::cancelled, "Grouped task publication is not available.",
             "Queue one complete generation on the model owner thread."});
    }
    if (!snapshot) {
        return foundation::Result<void>::failure({ErrorCode::invalid_argument,
                                                  "The grouped task snapshot is absent.",
                                                  "Retain the prior complete panel generation."});
    }
    auto valid = validateSnapshot(*snapshot);
    if (!valid) {
        return valid;
    }
    if (snapshot_) {
        const bool stale = snapshot->taskGeneration.value() < snapshot_->taskGeneration.value() ||
                           snapshot->associationGeneration < snapshot_->associationGeneration ||
                           snapshot->catalogGeneration < snapshot_->catalogGeneration ||
                           snapshot->pinGeneration < snapshot_->pinGeneration ||
                           snapshot->settingsGeneration < snapshot_->settingsGeneration ||
                           snapshot->widthGeneration < snapshot_->widthGeneration;
        if (stale) {
            return foundation::Result<void>::failure(
                {ErrorCode::cancelled, "The grouped task snapshot is stale.",
                 "Retain the newer complete panel generation."});
        }
        const bool sameGeneration =
            snapshot->taskGeneration == snapshot_->taskGeneration &&
            snapshot->associationGeneration == snapshot_->associationGeneration &&
            snapshot->catalogGeneration == snapshot_->catalogGeneration &&
            snapshot->pinGeneration == snapshot_->pinGeneration &&
            snapshot->settingsGeneration == snapshot_->settingsGeneration &&
            snapshot->widthGeneration == snapshot_->widthGeneration;
        if (sameGeneration) {
            if (snapshot.get() == snapshot_.get()) {
                return foundation::Result<void>::success();
            }
            return foundation::Result<void>::failure(
                {ErrorCode::validation_error, "Grouped task generations conflict.",
                 "Publish changed content with a newer owning generation."});
        }
    }

    std::vector<std::unique_ptr<GroupedTaskPresentation>> replacements;
    try {
        replacements.resize(snapshot->groups.size());
        for (std::size_t index = 0U; index < snapshot->groups.size(); ++index) {
            const auto &next = snapshot->groups[index];
            const auto prior = std::ranges::find(groups_, next.internalKey,
                                                 &GroupedTaskPresentation::internal_key_);
            if (prior == groups_.end() || !(*prior)->matches(next)) {
                replacements[index] = std::unique_ptr<GroupedTaskPresentation>(
                    new GroupedTaskPresentation(next, this));
            }
        }
    } catch (const std::exception &) {
        return foundation::Result<void>::failure(
            {ErrorCode::too_large, "Grouped task presentation rows could not be allocated.",
             "Retain the prior complete panel generation."});
    }

    applying_snapshot_ = true;
    emit publicationReconciliationStarted();
    for (std::size_t index = groups_.size(); index > 0U; --index) {
        const auto row = index - 1U;
        if (std::ranges::none_of(snapshot->groups, [this, row](const auto &group) {
                return group.internalKey == groups_[row]->internal_key_;
            })) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(row));
            endRemoveRows();
        }
    }
    for (std::size_t index = 0U; index < snapshot->groups.size(); ++index) {
        const auto &next = snapshot->groups[index];
        auto current =
            std::ranges::find(groups_, next.internalKey, &GroupedTaskPresentation::internal_key_);
        if (current == groups_.end()) {
            beginInsertRows({}, static_cast<int>(index), static_cast<int>(index));
            groups_.insert(groups_.begin() + static_cast<std::ptrdiff_t>(index),
                           std::move(replacements[index]));
            endInsertRows();
            continue;
        }
        auto currentIndex = static_cast<std::size_t>(std::distance(groups_.begin(), current));
        if (currentIndex != index) {
            beginMoveRows({}, static_cast<int>(currentIndex), static_cast<int>(currentIndex), {},
                          static_cast<int>(index));
            auto moved = std::move(groups_[currentIndex]);
            groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(currentIndex));
            groups_.insert(groups_.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
            endMoveRows();
            currentIndex = index;
        }
        if (replacements[index]) {
            groups_[currentIndex] = std::move(replacements[index]);
            emit dataChanged(this->index(static_cast<int>(currentIndex)),
                             this->index(static_cast<int>(currentIndex)), {Role::groupObject});
        }
    }
    snapshot_ = std::move(snapshot);
    applying_snapshot_ = false;
    emit publicationApplied();

    int groupIndex = -1;
    int memberIndex = -1;
    if (focus_lifetime_) {
        const auto recovered = groupForTaskLifetime(*snapshot_, *focus_lifetime_);
        if (recovered) {
            groupIndex = static_cast<int>(*recovered);
            const auto &members = snapshot_->groups[*recovered].members;
            const auto member =
                std::ranges::find(members, *focus_lifetime_, &PanelTaskMember::lifetime);
            memberIndex = member == members.end()
                              ? -1
                              : static_cast<int>(std::distance(members.begin(), member));
        }
    } else if (focus_group_key_) {
        const auto recovered =
            std::ranges::find(snapshot_->groups, *focus_group_key_, &PanelTaskGroup::internalKey);
        if (recovered != snapshot_->groups.end()) {
            groupIndex = static_cast<int>(std::distance(snapshot_->groups.begin(), recovered));
        }
    }
    if (groupIndex >= 0) {
        emit focusRecoveryRequested(groupIndex, memberIndex, focus_popup_open_);
    }
    focus_lifetime_.reset();
    focus_group_key_.reset();
    focus_popup_open_ = false;
    return foundation::Result<void>::success();
}

GroupedTaskPresentation *GroupedTaskPresentationModel::groupAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= groups_.size()) {
        return nullptr;
    }
    return groups_[static_cast<std::size_t>(row)].get();
}

bool GroupedTaskPresentationModel::rememberFocus(QObject *target, bool groupPopupOpen) {
    if (QThread::currentThread() != thread() || applying_snapshot_) {
        return false;
    }
    focus_lifetime_.reset();
    focus_group_key_.reset();
    focus_popup_open_ = groupPopupOpen;
    if (auto *member = qobject_cast<GroupedTaskMemberPresentation *>(target)) {
        focus_lifetime_ = member->lifetime_;
        return true;
    }
    if (auto *group = qobject_cast<GroupedTaskPresentation *>(target)) {
        if (!group->members_.empty()) {
            focus_lifetime_ = group->members_.front()->lifetime_;
        } else {
            focus_group_key_ = group->internal_key_;
        }
        return true;
    }
    return false;
}

bool GroupedTaskPresentationModel::canForward() const noexcept {
    return QThread::currentThread() == thread() && !applying_snapshot_ && snapshot_;
}

bool GroupedTaskPresentationModel::requestPrimary(GroupedTaskPresentation &group) {
    if (!canForward()) {
        return false;
    }
    switch (group.primary_action_) {
    case PanelTaskPrimaryAction::launch:
        return requestOpenNew(group);
    case PanelTaskPrimaryAction::activateOrMinimize:
        if (group.members_.size() != 1U) {
            return false;
        }
        return group.members_.front()->active() && !group.members_.front()->minimized()
                   ? requestMinimization(*group.members_.front())
                   : requestActivation(*group.members_.front());
    case PanelTaskPrimaryAction::openGroup:
        emit groupPopupRequested(&group);
        return true;
    case PanelTaskPrimaryAction::unavailable:
        return false;
    }
    return false;
}

bool GroupedTaskPresentationModel::requestOpenNew(const GroupedTaskPresentation &group) {
    if (!canForward() || !group.desktop_file_id_ || !group.launch_available_) {
        return false;
    }
    emit launchRequested(*group.desktop_file_id_);
    return true;
}

bool GroupedTaskPresentationModel::requestPinToggle(const GroupedTaskPresentation &group) {
    if (!canForward() || !group.desktop_file_id_) {
        return false;
    }
    emit pinChangeRequested(*group.desktop_file_id_, !group.pinned_);
    return true;
}

bool GroupedTaskPresentationModel::requestActivation(const GroupedTaskMemberPresentation &member) {
    if (!canForward()) {
        return false;
    }
    emit activationRequested(member.lifetime_, snapshot_->taskGeneration);
    return true;
}

bool GroupedTaskPresentationModel::requestMinimization(
    const GroupedTaskMemberPresentation &member) {
    if (!canForward() || member.minimized()) {
        return false;
    }
    emit minimizationRequested(member.lifetime_, snapshot_->taskGeneration);
    return true;
}

bool GroupedTaskPresentationModel::requestClose(const GroupedTaskMemberPresentation &member) {
    if (!canForward()) {
        return false;
    }
    emit closeRequested(member.lifetime_, snapshot_->taskGeneration);
    return true;
}

} // namespace prismdrake::shell::panel
