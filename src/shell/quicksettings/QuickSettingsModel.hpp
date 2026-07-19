#pragma once

#include "Generation.hpp"
#include "Result.hpp"
#include "SettingsSnapshot.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

namespace prismdrake::shell::quicksettings {

enum class AdapterState : std::uint8_t {
    available,
    unavailable,
    enabled,
    disabled,
    pending,
    error,
};

enum class CommandFailure : std::uint8_t {
    none,
    unavailable,
    busy,
    denied,
    failed,
};

struct CommandResult final {
    CommandFailure failure;

    [[nodiscard]] bool accepted() const noexcept { return failure == CommandFailure::none; }
};

/// Opaque correlation identity for one dispatched asynchronous adapter command.
struct CommandRequestId final {
    std::uint64_t value;

    friend bool operator==(const CommandRequestId &, const CommandRequestId &) = default;
};

/// Exact authoritative result delivered by an adapter after its asynchronous D-Bus reply.
struct CommandCompletion final {
    CommandFailure failure;
    std::optional<foundation::Generation> resultingGeneration;
    std::optional<std::uint64_t> ownerEpoch;
};

/// Fixed profile mutation boundary. Dispatch must be nonblocking and may call only
/// Settings1.RequestProfileChange. Its asynchronous reply is returned through
/// completeProfileRequest with the same private request identity.
class ProfileControlPort {
  public:
    virtual ~ProfileControlPort() = default;
    [[nodiscard]] virtual CommandResult requestProfile(config::Profile profile,
                                                       CommandRequestId requestId) = 0;
};

/// Fixed effective-DND mutation boundary. Dispatch must be nonblocking; notifyd remains
/// authoritative and its asynchronous reply is returned through completeDoNotDisturbRequest.
class DoNotDisturbControlPort {
  public:
    virtual ~DoNotDisturbControlPort() = default;
    [[nodiscard]] virtual CommandResult requestDoNotDisturb(bool enabled,
                                                            CommandRequestId requestId) = 0;
};

/// Narrow injected bridge to the single notification-center surface owned by the focus router.
class NotificationCenterPort {
  public:
    virtual ~NotificationCenterPort() = default;
    [[nodiscard]] virtual CommandResult requestOpen() = 0;
};

/// Fixed logout-request boundary. Implementations may request but never confirm logout.
class LogoutControlPort {
  public:
    virtual ~LogoutControlPort() = default;
    [[nodiscard]] virtual CommandResult requestLogout() = 0;
};

enum class SessionPresentationState : std::uint8_t {
    idle,
    confirmationRequested,
    stopping,
    failed,
};

enum class DiagnosticSummary : std::uint8_t {
    unavailable,
    healthy,
    degraded,
    failed,
};

/// Complete notification-owner publication required by the quick-settings DND mirror.
struct DoNotDisturbPublication final {
    std::uint64_t ownerEpoch;
    foundation::Generation generation;
    bool effective;

    friend bool operator==(const DoNotDisturbPublication &,
                           const DoNotDisturbPublication &) = default;
};

/// Read-only QML projection of one typed quick-settings capability.
class QuickSettingAdapter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString kindId READ kindId CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QString stateId READ stateId NOTIFY changed)
    Q_PROPERTY(QString stateText READ stateText NOTIFY changed)
    Q_PROPERTY(QString valueId READ valueId NOTIFY changed)
    Q_PROPERTY(QString valueText READ valueText NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool interactive READ interactive NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    Q_PROPERTY(bool checked READ checked NOTIFY changed)

  public:
    [[nodiscard]] const QString &kindId() const noexcept { return kind_id_; }
    [[nodiscard]] const QString &title() const noexcept { return title_; }
    [[nodiscard]] const QString &description() const noexcept { return description_; }
    [[nodiscard]] QString stateId() const;
    [[nodiscard]] QString stateText() const;
    [[nodiscard]] const QString &valueId() const noexcept { return value_id_; }
    [[nodiscard]] const QString &valueText() const noexcept { return value_text_; }
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool interactive() const noexcept;
    [[nodiscard]] bool enabled() const noexcept { return state_ == AdapterState::enabled; }
    [[nodiscard]] bool pending() const noexcept { return state_ == AdapterState::pending; }
    [[nodiscard]] bool failed() const noexcept { return state_ == AdapterState::error; }
    [[nodiscard]] bool checked() const noexcept { return checked_; }
    [[nodiscard]] AdapterState state() const noexcept { return state_; }

  signals:
    void changed();

  private:
    QuickSettingAdapter(QString kindId, QString title, QString description, QObject *parent);
    void publish(AdapterState state, QString description, QString valueText = {},
                 bool checked = false, QString valueId = {});

    QString kind_id_;
    QString title_;
    QString description_;
    QString value_id_;
    QString value_text_;
    AdapterState state_{AdapterState::unavailable};
    bool checked_{false};

    friend class QuickSettingsModel;
};

/// Display-free state reducer and typed command boundary for the installed quick-settings surface.
///
/// Only complete settings and notification publications are accepted. Runtime generations and
/// owner epochs remain private to C++; QML receives categorical state and literal display text.
class QuickSettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QuickSettingAdapter *profile READ profile CONSTANT)
    Q_PROPERTY(QuickSettingAdapter *doNotDisturb READ doNotDisturb CONSTANT)
    Q_PROPERTY(QuickSettingAdapter *notificationCenter READ notificationCenter CONSTANT)
    Q_PROPERTY(QuickSettingAdapter *logout READ logout CONSTANT)
    Q_PROPERTY(QuickSettingAdapter *diagnostics READ diagnostics CONSTANT)

  public:
    explicit QuickSettingsModel(QObject *parent = nullptr);

    [[nodiscard]] QuickSettingAdapter *profile() noexcept { return &profile_; }
    [[nodiscard]] QuickSettingAdapter *doNotDisturb() noexcept { return &do_not_disturb_; }
    [[nodiscard]] QuickSettingAdapter *notificationCenter() noexcept {
        return &notification_center_;
    }
    [[nodiscard]] QuickSettingAdapter *logout() noexcept { return &logout_; }
    [[nodiscard]] QuickSettingAdapter *diagnostics() noexcept { return &diagnostics_; }

    void setProfileControlPort(ProfileControlPort *port) noexcept;
    void setDoNotDisturbControlPort(DoNotDisturbControlPort *port) noexcept;
    void setNotificationCenterPort(NotificationCenterPort *port) noexcept;
    void setLogoutControlPort(LogoutControlPort *port) noexcept;

    [[nodiscard]] foundation::Result<void>
    applySettingsSnapshot(std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot);
    void clearSettingsOwner() noexcept;

    [[nodiscard]] foundation::Result<void>
    applyDoNotDisturbPublication(DoNotDisturbPublication publication);
    void clearNotificationOwner() noexcept;

    [[nodiscard]] foundation::Result<void> completeProfileRequest(CommandRequestId requestId,
                                                                  CommandCompletion completion);
    [[nodiscard]] foundation::Result<void>
    completeDoNotDisturbRequest(CommandRequestId requestId, CommandCompletion completion);

    void setNotificationCenterAvailable(bool available) noexcept;
    void observeNotificationCenterVisible(bool visible) noexcept;
    void setSessionAvailable(bool available) noexcept;
    void observeSessionState(SessionPresentationState state) noexcept;
    void applyDiagnosticSummary(DiagnosticSummary summary) noexcept;

    Q_INVOKABLE bool requestLustreProfile();
    Q_INVOKABLE bool requestForgeProfile();
    Q_INVOKABLE bool requestDoNotDisturb(bool enabled);
    Q_INVOKABLE bool requestNotificationCenter();
    Q_INVOKABLE bool requestLogout();

  private:
    [[nodiscard]] bool requestProfile(config::Profile profile);
    void publishProfileState();
    void publishDoNotDisturbState();
    void publishNotificationCenterState();
    void publishLogoutState();
    static AdapterState commandFailureState(CommandFailure failure) noexcept;
    [[nodiscard]] CommandRequestId allocateRequestId() noexcept;
    void reconcilePendingProfile();
    void reconcilePendingDoNotDisturb();

    struct PendingProfileRequest final {
        CommandRequestId id;
        config::Profile requested;
        std::optional<foundation::Generation> resultingGeneration;
    };

    struct PendingDoNotDisturbRequest final {
        CommandRequestId id;
        bool requested;
        std::optional<std::uint64_t> ownerEpoch;
        std::optional<foundation::Generation> resultingGeneration;
    };

    QuickSettingAdapter profile_;
    QuickSettingAdapter do_not_disturb_;
    QuickSettingAdapter notification_center_;
    QuickSettingAdapter logout_;
    QuickSettingAdapter diagnostics_;

    ProfileControlPort *profile_port_{nullptr};
    DoNotDisturbControlPort *do_not_disturb_port_{nullptr};
    NotificationCenterPort *notification_center_port_{nullptr};
    LogoutControlPort *logout_port_{nullptr};

    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> settings_snapshot_;
    std::optional<PendingProfileRequest> pending_profile_;
    std::optional<DoNotDisturbPublication> do_not_disturb_publication_;
    std::optional<PendingDoNotDisturbRequest> pending_do_not_disturb_;
    bool notification_center_available_{false};
    bool notification_center_visible_{false};
    bool notification_center_pending_{false};
    bool session_available_{false};
    SessionPresentationState session_state_{SessionPresentationState::idle};
    bool logout_pending_{false};
    std::uint64_t next_request_id_{1U};
};

} // namespace prismdrake::shell::quicksettings
