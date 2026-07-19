#pragma once

#include "ApplicationSearch.hpp"
#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace prismdrake::shell::launcher {

class LauncherPresentationModel;
class LauncherResultPresentation;

enum class ShellDestination : std::uint8_t { quickSettings, notificationCenter };

/// Exact C++ controller intent. None of these identity fields is a QML property.
struct ApplicationLaunchIntent final {
    prismdrake::launcher::DesktopFileId desktopFileId;
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
};

struct ApplicationActionIntent final {
    prismdrake::launcher::DesktopFileId desktopFileId;
    std::string actionId;
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
};

struct ApplicationPinIntent final {
    prismdrake::launcher::DesktopFileId desktopFileId;
    bool pin;
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
};

struct ShellDestinationIntent final {
    ShellDestination destination;
    std::uint64_t requestGeneration;
};

/// Private C++ icon request; QML receives only the opaque key and resolved image data.
struct LauncherIconIntent final {
    QString opaqueKey;
    prismdrake::launcher::DesktopFileId desktopFileId;
    QString iconField;
    std::uint64_t catalogGeneration;
};

class LauncherActionPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)

  public:
    [[nodiscard]] const QString &name() const noexcept { return name_; }
    Q_INVOKABLE bool request();

  private:
    LauncherActionPresentation(std::string identifier, QString name,
                               LauncherResultPresentation *owner);

    const std::string identifier_;
    const QString name_;
    LauncherResultPresentation *const owner_;

    friend class LauncherResultPresentation;
    friend class LauncherPresentationModel;
};

/// Immutable literal presentation for one eligible desktop-file identity.
/// A retired object remains alive through structural model reconciliation, but launch
/// requests are rejected as soon as its identity leaves the current complete publication.
class LauncherResultPresentation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString genericName READ genericName CONSTANT)
    Q_PROPERTY(QString comment READ comment CONSTANT)
    Q_PROPERTY(QString kindId READ kindId CONSTANT)
    Q_PROPERTY(QString iconKey READ iconKey CONSTANT)
    Q_PROPERTY(QUrl iconSource READ iconSource NOTIFY iconSourceChanged)
    Q_PROPERTY(bool terminalRequired READ terminalRequired CONSTANT)
    Q_PROPERTY(bool canPin READ canPin CONSTANT)
    Q_PROPERTY(bool pinned READ pinned NOTIFY pinnedChanged)
    Q_PROPERTY(QVariantList actions READ actions CONSTANT)
    Q_PROPERTY(int actionCount READ actionCount CONSTANT)

  public:
    [[nodiscard]] const QString &name() const noexcept { return name_; }
    [[nodiscard]] const QString &genericName() const noexcept { return generic_name_; }
    [[nodiscard]] const QString &comment() const noexcept { return comment_; }
    [[nodiscard]] const QString &icon() const noexcept { return icon_; }
    [[nodiscard]] const QString &kindId() const noexcept { return kind_id_; }
    [[nodiscard]] const QString &iconKey() const noexcept { return icon_key_; }
    [[nodiscard]] const QUrl &iconSource() const noexcept { return icon_source_; }
    [[nodiscard]] bool terminalRequired() const noexcept { return terminal_required_; }
    [[nodiscard]] bool canPin() const noexcept { return desktop_file_id_.has_value(); }
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }
    [[nodiscard]] QVariantList actions() const;
    [[nodiscard]] int actionCount() const noexcept;
    Q_INVOKABLE QObject *actionAt(int index) const noexcept;

    /// Requests controller handling only; this never expands Exec or starts a process.
    Q_INVOKABLE bool requestLaunch();
    Q_INVOKABLE bool requestPinToggle();

  signals:
    void pinnedChanged();
    void iconSourceChanged();

  private:
    LauncherResultPresentation(const prismdrake::launcher::DiscoveredDesktopEntry &entry,
                               QString opaqueIconKey, bool pinned,
                               LauncherPresentationModel *owner);
    LauncherResultPresentation(ShellDestination destination, QString name, QString comment,
                               QString opaqueIconKey, LauncherPresentationModel *owner);

    [[nodiscard]] bool
    matches(const prismdrake::launcher::DiscoveredDesktopEntry &entry) const noexcept;
    [[nodiscard]] bool requestAction(const LauncherActionPresentation &action);
    void setPinned(bool pinned);
    void setResolvedIcon(const QImage &image);

    const std::optional<prismdrake::launcher::DesktopFileId> desktop_file_id_;
    const std::optional<ShellDestination> destination_;
    LauncherPresentationModel *const owner_;
    const QString name_;
    const QString generic_name_;
    const QString comment_;
    const QString icon_;
    const QString kind_id_;
    const QString icon_key_;
    const bool terminal_required_;
    std::vector<std::unique_ptr<LauncherActionPresentation>> actions_;
    QUrl icon_source_;
    bool pinned_{false};

    friend class LauncherPresentationModel;
    friend class LauncherActionPresentation;
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
                  std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot> search,
                  const prismdrake::launcher::ApplicationSearchQuery *query = nullptr);

    [[nodiscard]] foundation::Result<void>
    applyPinSnapshot(std::uint64_t generation,
                     const std::vector<prismdrake::launcher::DesktopFileId> &pins);
    void applyResolvedIcon(const QString &opaqueKey, const QImage &image);
    void requestAllIcons();
    Q_INVOKABLE bool requestRefresh();

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
    void actionRequested(const prismdrake::shell::launcher::ApplicationActionIntent &intent);
    void pinRequested(const prismdrake::shell::launcher::ApplicationPinIntent &intent);
    void destinationRequested(const prismdrake::shell::launcher::ShellDestinationIntent &intent);
    void iconRequested(const prismdrake::shell::launcher::LauncherIconIntent &intent);
    void refreshRequested();

  private:
    friend class LauncherResultPresentation;

    [[nodiscard]] bool requestLaunch(const LauncherResultPresentation &result);
    [[nodiscard]] bool requestAction(const LauncherResultPresentation &result,
                                     const LauncherActionPresentation &action);
    [[nodiscard]] bool requestPinToggle(LauncherResultPresentation &result);
    [[nodiscard]] bool containsCurrentIdentity(
        const prismdrake::launcher::DesktopFileId &desktopFileId) const noexcept;

    std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog_;
    std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot> search_;
    std::vector<std::unique_ptr<LauncherResultPresentation>> results_;
    ViewState view_state_{ViewState::loading};
    bool truncated_{false};
    bool applying_snapshot_{false};
    std::set<std::string> pinned_ids_;
    std::uint64_t pin_generation_{0U};
    std::uint64_t next_icon_key_{1U};
};

} // namespace prismdrake::shell::launcher

Q_DECLARE_METATYPE(prismdrake::shell::launcher::ApplicationLaunchIntent)
Q_DECLARE_METATYPE(prismdrake::shell::launcher::ApplicationActionIntent)
Q_DECLARE_METATYPE(prismdrake::shell::launcher::ApplicationPinIntent)
Q_DECLARE_METATYPE(prismdrake::shell::launcher::ShellDestinationIntent)
Q_DECLARE_METATYPE(prismdrake::shell::launcher::LauncherIconIntent)
