#pragma once

#include "Result.hpp"
#include "SdBusApi.hpp"
#include "SettingsSnapshot.hpp"

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class QSocketNotifier;

namespace prismdrake::shell::settings {

/// Nonblocking Qt event-loop client for the authoritative settingsd snapshot contract.
///
/// Consumers observe snapshotChanged() and synchronously copy the immutable shared pointer from
/// currentSnapshot(). JSON transport bytes and mutable partial settings are never exposed.
class SettingsSnapshotClient final : public QObject {
    Q_OBJECT

  public:
    enum class State : std::uint8_t { stopped, connecting, unavailable, fetching, ready, failed };
    Q_ENUM(State)

    explicit SettingsSnapshotClient(QObject *parent = nullptr);
    ~SettingsSnapshotClient() override;

    SettingsSnapshotClient(const SettingsSnapshotClient &) = delete;
    SettingsSnapshotClient &operator=(const SettingsSnapshotClient &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::shared_ptr<const prismdrake::settings::SettingsSnapshot> &
    currentSnapshot() const noexcept {
        return snapshot_;
    }
    [[nodiscard]] const std::optional<foundation::Error> &lastError() const noexcept {
        return lastError_;
    }

  signals:
    void snapshotChanged();
    void stateChanged();

  private:
    static int matchInstalled(sd_bus_message *message, void *userdata,
                              sd_bus_error *returnError) noexcept;
    static int nameOwnerChanged(sd_bus_message *message, void *userdata,
                                sd_bus_error *returnError) noexcept;
    static int generationChanged(sd_bus_message *message, void *userdata,
                                 sd_bus_error *returnError) noexcept;
    static int ownerReply(sd_bus_message *message, void *userdata,
                          sd_bus_error *returnError) noexcept;
    static int snapshotReply(sd_bus_message *message, void *userdata,
                             sd_bus_error *returnError) noexcept;

    [[nodiscard]] foundation::Result<void> installMatches();
    [[nodiscard]] foundation::Result<void> queryOwner();
    [[nodiscard]] foundation::Result<void> requestSnapshot();
    void dispatchBus() noexcept;
    void updateEventSources() noexcept;
    void handleBusFailure() noexcept;
    void invalidateOwnerEpoch(foundation::Error error) noexcept;
    void applyOwner(std::string owner) noexcept;
    void clearSnapshot() noexcept;
    void setState(State state) noexcept;
    void setFailure(foundation::Error error, bool retainSnapshot) noexcept;
    void schedulePendingFetch() noexcept;

    ipc::sdbus::Bus bus_;
    ipc::sdbus::Slot ownerMatch_;
    ipc::sdbus::Slot generationMatch_;
    ipc::sdbus::Slot ownerQuery_;
    ipc::sdbus::Slot snapshotCall_;
    std::unique_ptr<QSocketNotifier> readNotifier_;
    std::unique_ptr<QSocketNotifier> writeNotifier_;
    QTimer busTimer_;
    State state_{State::stopped};
    std::shared_ptr<const prismdrake::settings::SettingsSnapshot> snapshot_;
    std::optional<foundation::Error> lastError_;
    std::string owner_;
    std::string requestedOwner_;
    std::uint8_t pendingMatchInstalls_{0U};
    bool fetchPending_{false};
    bool dispatchQueued_{false};
};

} // namespace prismdrake::shell::settings
