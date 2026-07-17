#pragma once

#include "ApplicationSearch.hpp"
#include "DesktopFileId.hpp"
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

namespace prismdrake::shell::launcher {

class LauncherPresentationModel;

/// Exact C++ controller intent. None of these identity fields is a QML property.
struct ApplicationLaunchIntent final {
    prismdrake::launcher::DesktopFileId desktopFileId;
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
};

/// Immutable literal presentation for one eligible desktop-file identity.
class LauncherResultPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString genericName READ genericName CONSTANT)
    Q_PROPERTY(QString comment READ comment CONSTANT)
    Q_PROPERTY(QString icon READ icon CONSTANT)
    Q_PROPERTY(bool terminalRequired READ terminalRequired CONSTANT)

  public:
    [[nodiscard]] const QString &name() const noexcept { return name_; }
    [[nodiscard]] const QString &genericName() const noexcept { return generic_name_; }
    [[nodiscard]] const QString &comment() const noexcept { return comment_; }
    [[nodiscard]] const QString &icon() const noexcept { return icon_; }
    [[nodiscard]] bool terminalRequired() const noexcept { return terminal_required_; }

    /// Requests controller handling only; this never expands Exec or starts a process.
    Q_INVOKABLE bool requestLaunch();

  private:
    LauncherResultPresentation(const prismdrake::launcher::DiscoveredDesktopEntry &entry,
                               LauncherPresentationModel *owner);

    [[nodiscard]] bool
    matches(const prismdrake::launcher::DiscoveredDesktopEntry &entry) const noexcept;

    const prismdrake::launcher::DesktopFileId desktop_file_id_;
    LauncherPresentationModel *const owner_;
    const QString name_;
    const QString generic_name_;
    const QString comment_;
    const QString icon_;
    const bool terminal_required_;

    friend class LauncherPresentationModel;
};

/// Passive mirror of one matching immutable application catalog/search publication.
class LauncherPresentationModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(ViewState viewState READ viewState NOTIFY publicationApplied)
    Q_PROPERTY(QString stateId READ stateId NOTIFY publicationApplied)
    Q_PROPERTY(QString stateLabel READ stateLabel NOTIFY publicationApplied)
    Q_PROPERTY(bool truncated READ truncated NOTIFY publicationApplied)

  public:
    enum class ViewState : std::uint8_t {
        loading,
        results,
        emptyCatalog,
        noResults,
        error,
    };
    Q_ENUM(ViewState)

    enum Role : int {
        resultObject = Qt::UserRole + 1,
    };
    Q_ENUM(Role)

    explicit LauncherPresentationModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] foundation::Result<void>
    applySnapshot(std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog,
                  std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot> search);

    [[nodiscard]] LauncherResultPresentation *resultAt(int row) const noexcept;
    [[nodiscard]] ViewState viewState() const noexcept { return view_state_; }
    [[nodiscard]] QString stateId() const;
    [[nodiscard]] QString stateLabel() const;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] bool isApplyingSnapshot() const noexcept { return applying_snapshot_; }
    [[nodiscard]] const std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> &
    currentCatalog() const noexcept {
        return catalog_;
    }
    [[nodiscard]] const std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot> &
    currentSearch() const noexcept {
        return search_;
    }

  signals:
    void publicationReconciliationStarted();
    void publicationApplied();
    void launchRequested(const prismdrake::shell::launcher::ApplicationLaunchIntent &intent);

  private:
    friend class LauncherResultPresentation;

    [[nodiscard]] bool requestLaunch(const LauncherResultPresentation &result);
    [[nodiscard]] bool containsCurrentIdentity(
        const prismdrake::launcher::DesktopFileId &desktopFileId) const noexcept;

    std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog_;
    std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot> search_;
    std::vector<std::unique_ptr<LauncherResultPresentation>> results_;
    ViewState view_state_{ViewState::loading};
    bool truncated_{false};
    bool applying_snapshot_{false};
};

} // namespace prismdrake::shell::launcher

Q_DECLARE_METATYPE(prismdrake::shell::launcher::ApplicationLaunchIntent)
