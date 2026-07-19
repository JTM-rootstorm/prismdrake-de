#pragma once

#include "NotificationCenterState.hpp"
#include "Result.hpp"
#include "SdBusApi.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class QSocketNotifier;

namespace prismdrake::shell::notificationcenter {

/// Nonblocking owner-epoch client for the authoritative NotificationCenter1 state.
///
/// StateChanged is only a refetch hint. Consumers receive complete immutable
/// snapshots and never observe a partially applied mutation.
class NotificationCenterClient final : public QObject {
    Q_OBJECT

  public:
    enum class State : std::uint8_t {
        stopped,
        connecting,
        unavailable,
        fetching,
        ready,
        mutating,
        failed
    };
    Q_ENUM(State)

    enum class Mutation : std::uint8_t {
        none,
        setDoNotDisturb,
        clearHistory,
        dismiss,
        invokeAction
    };
    Q_ENUM(Mutation)

    explicit NotificationCenterClient(QObject *parent = nullptr);
    ~NotificationCenterClient() override;

    NotificationCenterClient(const NotificationCenterClient &) = delete;
    NotificationCenterClient &operator=(const NotificationCenterClient &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] Mutation activeMutation() const noexcept { return mutation_; }
    [[nodiscard]] const NotificationCenterSnapshotPtr &currentSnapshot() const noexcept {
        return snapshot_;
    }
    [[nodiscard]] const std::optional<foundation::Error> &lastError() const noexcept {
        return lastError_;
    }

    [[nodiscard]] foundation::Result<void> setDoNotDisturb(bool enabled);
    [[nodiscard]] foundation::Result<void> clearHistory();
    [[nodiscard]] foundation::Result<void> dismiss(NotificationRecordCommandIdentity identity);
    [[nodiscard]] foundation::Result<void> invokeAction(NotificationRecordCommandIdentity identity,
                                                        const QString &actionId);

  signals:
    void snapshotChanged();
    void stateChanged();
    void mutationCompleted(Mutation mutation, bool confirmed);

  private:
    static int matchInstalled(sd_bus_message *message, void *userdata,
                              sd_bus_error *returnError) noexcept;
    static int nameOwnerChanged(sd_bus_message *message, void *userdata,
                                sd_bus_error *returnError) noexcept;
    static int stateChangedSignal(sd_bus_message *message, void *userdata,
                                  sd_bus_error *returnError) noexcept;
    static int ownerReply(sd_bus_message *message, void *userdata,
                          sd_bus_error *returnError) noexcept;
    static int snapshotReply(sd_bus_message *message, void *userdata,
                             sd_bus_error *returnError) noexcept;
    static int mutationReply(sd_bus_message *message, void *userdata,
                             sd_bus_error *returnError) noexcept;

    [[nodiscard]] foundation::Result<void> installMatches();
    [[nodiscard]] foundation::Result<void> queryOwner();
    [[nodiscard]] foundation::Result<void> requestSnapshot();
    [[nodiscard]] foundation::Result<void> beginMutation(Mutation mutation, const char *member,
                                                         const char *signature = nullptr, ...);
    [[nodiscard]] foundation::Result<const NotificationCenterRecord *>
    currentRecord(NotificationRecordCommandIdentity identity) const;
    void dispatchBus() noexcept;
    void updateEventSources() noexcept;
    void handleBusFailure() noexcept;
    void invalidateOwnerEpoch(foundation::Error error) noexcept;
    void applyOwner(std::string owner) noexcept;
    void clearSnapshot() noexcept;
    void setState(State state) noexcept;
    void setFailure(foundation::Error error, bool retainSnapshot) noexcept;
    void schedulePendingFetch() noexcept;
    void finishMutation(bool confirmed, bool refetch) noexcept;

    ipc::sdbus::Bus bus_;
    ipc::sdbus::Slot ownerMatch_;
    ipc::sdbus::Slot stateChangedMatch_;
    ipc::sdbus::Slot ownerQuery_;
    ipc::sdbus::Slot snapshotCall_;
    ipc::sdbus::Slot mutationCall_;
    std::unique_ptr<QSocketNotifier> readNotifier_;
    std::unique_ptr<QSocketNotifier> writeNotifier_;
    QTimer busTimer_;
    State state_{State::stopped};
    Mutation mutation_{Mutation::none};
    NotificationCenterSnapshotPtr snapshot_;
    std::optional<foundation::Error> lastError_;
    std::string owner_;
    std::string requestedOwner_;
    std::string mutationOwner_;
    std::uint8_t pendingMatchInstalls_{0U};
    bool fetchPending_{false};
    bool dispatchQueued_{false};
};

} // namespace prismdrake::shell::notificationcenter
