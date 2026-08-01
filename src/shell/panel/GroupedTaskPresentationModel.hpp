#pragma once

#include "GroupedTaskModel.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::shell::panel {

class GroupedTaskPresentationModel;
class GroupedTaskPresentation;

class GroupedTaskMemberPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool minimized READ minimized CONSTANT)
    Q_PROPERTY(bool urgent READ urgent CONSTANT)
    Q_PROPERTY(bool modal READ modal CONSTANT)
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

  public:
    [[nodiscard]] const QString &title() const noexcept { return title_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool minimized() const noexcept { return minimized_; }
    [[nodiscard]] bool urgent() const noexcept { return urgent_; }
    [[nodiscard]] bool modal() const noexcept { return modal_; }
    [[nodiscard]] const QString &statusText() const noexcept { return status_text_; }

    Q_INVOKABLE bool requestActivation();
    Q_INVOKABLE bool requestMinimization();
    Q_INVOKABLE bool requestClose();

  private:
    GroupedTaskMemberPresentation(const PanelTaskMember &member,
                                  GroupedTaskPresentationModel *owner,
                                  GroupedTaskPresentation *group);
    [[nodiscard]] bool matches(const PanelTaskMember &member) const;

    const x11::TaskLifetimeId lifetime_;
    GroupedTaskPresentationModel *const owner_;
    GroupedTaskPresentation *const group_;
    const QString title_;
    const bool active_;
    const bool minimized_;
    const bool urgent_;
    const bool modal_;
    const QString status_text_;

    friend class GroupedTaskPresentation;
    friend class GroupedTaskPresentationModel;
};

class GroupedTaskPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString displayName READ displayName CONSTANT)
    Q_PROPERTY(QString iconName READ iconName CONSTANT)
    Q_PROPERTY(bool pinned READ pinned CONSTANT)
    Q_PROPERTY(bool launchAvailable READ launchAvailable CONSTANT)
    Q_PROPERTY(bool running READ running CONSTANT)
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(bool minimized READ minimized CONSTANT)
    Q_PROPERTY(bool urgent READ urgent CONSTANT)
    Q_PROPERTY(bool modal READ modal CONSTANT)
    Q_PROPERTY(int windowCount READ windowCount CONSTANT)
    Q_PROPERTY(bool primaryOpensGroup READ primaryOpensGroup CONSTANT)
    Q_PROPERTY(QString statusText READ statusText CONSTANT)
    Q_PROPERTY(QVariantList members READ members CONSTANT)

  public:
    [[nodiscard]] const QString &displayName() const noexcept { return display_name_; }
    [[nodiscard]] const QString &iconName() const noexcept { return icon_name_; }
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }
    [[nodiscard]] bool launchAvailable() const noexcept { return launch_available_; }
    [[nodiscard]] bool running() const noexcept { return !members_.empty(); }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool minimized() const noexcept { return minimized_; }
    [[nodiscard]] bool urgent() const noexcept { return urgent_; }
    [[nodiscard]] bool modal() const noexcept { return modal_; }
    [[nodiscard]] int windowCount() const noexcept { return static_cast<int>(members_.size()); }
    [[nodiscard]] bool primaryOpensGroup() const noexcept {
        return primary_action_ == PanelTaskPrimaryAction::openGroup;
    }
    [[nodiscard]] const QString &statusText() const noexcept { return status_text_; }
    [[nodiscard]] QVariantList members() const;

    Q_INVOKABLE bool requestPrimaryAction();
    Q_INVOKABLE bool requestOpenNew();
    Q_INVOKABLE bool requestPinToggle();
    Q_INVOKABLE bool requestOnlyWindowMinimization();
    Q_INVOKABLE bool requestOnlyWindowClose();

  private:
    GroupedTaskPresentation(const PanelTaskGroup &group, GroupedTaskPresentationModel *owner);
    [[nodiscard]] bool matches(const PanelTaskGroup &group) const;

    GroupedTaskPresentationModel *const owner_;
    const PanelTaskGroupKind kind_;
    const std::string internal_key_;
    const std::optional<launcher::DesktopFileId> desktop_file_id_;
    const QString display_name_;
    const QString icon_name_;
    const bool pinned_;
    const bool launch_available_;
    const bool active_;
    const bool minimized_;
    const bool urgent_;
    const bool modal_;
    const PanelTaskPrimaryAction primary_action_;
    const QString status_text_;
    std::vector<std::unique_ptr<GroupedTaskMemberPresentation>> members_;

    friend class GroupedTaskPresentationModel;
};

/// Qt mirror that keeps all task lifetimes, generations, and desktop IDs private from QML.
class GroupedTaskPresentationModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int { groupObject = Qt::UserRole + 1 };
    Q_ENUM(Role)

    explicit GroupedTaskPresentationModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const GroupedTaskSnapshot> snapshot);
    /// Ends the current settings-owner epoch without discarding task rows. Forwarding remains
    /// disabled until a complete snapshot from the replacement owner is applied.
    void clearSettingsOwner() noexcept;
    [[nodiscard]] GroupedTaskPresentation *groupAt(int row) const noexcept;
    [[nodiscard]] std::shared_ptr<const GroupedTaskSnapshot> currentSnapshot() const noexcept {
        return snapshot_;
    }

    /// Records an opaque group presentation as the keyboard recovery target.
    Q_INVOKABLE bool rememberTaskFocus(QObject *group);
    /// Records an opaque member presentation inside an open group popup.
    Q_INVOKABLE bool rememberGroupPopupFocus(QObject *member);
    /// Records an opaque group presentation and exact grouped-menu action.
    Q_INVOKABLE bool rememberContextMenuFocus(QObject *group, int actionIndex);

  signals:
    void publicationReconciliationStarted();
    void publicationApplied();
    void taskFocusRecoveryRequested(int groupIndex);
    void groupPopupFocusRecoveryRequested(int groupIndex, int memberIndex);
    void contextMenuFocusRecoveryRequested(int groupIndex, int actionIndex);
    void launchRequested(prismdrake::launcher::DesktopFileId desktopFileId);
    void pinChangeRequested(prismdrake::launcher::DesktopFileId desktopFileId, bool pinned);
    void activationRequested(prismdrake::x11::TaskLifetimeId lifetime,
                             prismdrake::x11::TaskModelGeneration taskGeneration);
    void minimizationRequested(prismdrake::x11::TaskLifetimeId lifetime,
                               prismdrake::x11::TaskModelGeneration taskGeneration);
    void closeRequested(prismdrake::x11::TaskLifetimeId lifetime,
                        prismdrake::x11::TaskModelGeneration taskGeneration);
    void groupPopupRequested(QObject *group);

  private:
    [[nodiscard]] bool canForward() const noexcept;
    [[nodiscard]] bool requestPrimary(GroupedTaskPresentation &group);
    [[nodiscard]] bool requestOpenNew(const GroupedTaskPresentation &group);
    [[nodiscard]] bool requestPinToggle(const GroupedTaskPresentation &group);
    [[nodiscard]] bool requestActivation(const GroupedTaskMemberPresentation &member);
    [[nodiscard]] bool requestMinimization(const GroupedTaskMemberPresentation &member);
    [[nodiscard]] bool requestClose(const GroupedTaskMemberPresentation &member);

    std::shared_ptr<const GroupedTaskSnapshot> snapshot_;
    std::vector<std::unique_ptr<GroupedTaskPresentation>> groups_;
    enum class FocusRecoveryKind { taskRoot, groupPopup, contextMenu };
    struct FocusRecovery final {
        FocusRecoveryKind kind;
        std::optional<x11::TaskLifetimeId> lifetime;
        std::optional<std::string> groupKey;
        int actionIndex{-1};
    };
    std::optional<FocusRecovery> focus_recovery_;
    bool applying_snapshot_{false};
    bool accepting_focus_capture_{false};

    friend class GroupedTaskMemberPresentation;
    friend class GroupedTaskPresentation;
};

} // namespace prismdrake::shell::panel

Q_DECLARE_METATYPE(prismdrake::launcher::DesktopFileId)
