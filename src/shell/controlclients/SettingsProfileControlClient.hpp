#pragma once

#include "QtSessionBusDispatcher.hpp"
#include "QuickSettingsModel.hpp"
#include "SdBusApi.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace prismdrake::shell::controlclients {

/// Nonblocking Settings1.RequestProfileChange adapter for QuickSettingsModel.
///
/// The callback always runs on a later Qt event-loop turn for an accepted request. It receives
/// exactly the CommandRequestId supplied by the caller and either the authoritative generation
/// returned by the same settings-service owner epoch or a closed failure without a generation.
class SettingsProfileControlClient final : public QObject,
                                           public quicksettings::ProfileControlPort {
    Q_OBJECT

  public:
    enum class State : std::uint8_t { stopped, connecting, unavailable, available, failed };
    Q_ENUM(State)

    using CompletionCallback =
        std::function<void(quicksettings::CommandRequestId, quicksettings::CommandCompletion)>;

    explicit SettingsProfileControlClient(CompletionCallback completionCallback,
                                          QObject *parent = nullptr);
    ~SettingsProfileControlClient() override;

    SettingsProfileControlClient(const SettingsProfileControlClient &) = delete;
    SettingsProfileControlClient &operator=(const SettingsProfileControlClient &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t ownerEpoch() const noexcept { return owner_epoch_; }

    [[nodiscard]] quicksettings::CommandResult
    requestProfile(config::Profile profile, quicksettings::CommandRequestId requestId) override;

  signals:
    void stateChanged();

  private:
    struct PendingRequest final {
        quicksettings::CommandRequestId requestId;
        std::uint64_t ownerEpoch;
        std::string owner;
    };

    static int matchInstalled(sd_bus_message *message, void *userdata,
                              sd_bus_error *returnError) noexcept;
    static int nameOwnerChanged(sd_bus_message *message, void *userdata,
                                sd_bus_error *returnError) noexcept;
    static int ownerReply(sd_bus_message *message, void *userdata,
                          sd_bus_error *returnError) noexcept;
    static int profileReply(sd_bus_message *message, void *userdata,
                            sd_bus_error *returnError) noexcept;

    [[nodiscard]] foundation::Result<void> installMatch();
    [[nodiscard]] foundation::Result<void> queryOwner();
    void handleBusFailure() noexcept;
    void applyOwner(std::string owner) noexcept;
    void invalidateOwner(quicksettings::CommandFailure failure, State state) noexcept;
    void finishPending(quicksettings::CommandCompletion completion) noexcept;
    void setState(State state) noexcept;
    [[nodiscard]] bool advanceOwnerEpoch() noexcept;

    CompletionCallback completion_callback_;
    detail::QtSessionBusDispatcher dispatcher_;
    ipc::sdbus::Slot owner_match_;
    ipc::sdbus::Slot owner_query_;
    ipc::sdbus::Slot profile_call_;
    std::optional<PendingRequest> pending_;
    State state_{State::stopped};
    std::string owner_;
    std::uint64_t owner_epoch_{0U};
    bool match_pending_{false};
    bool destroying_{false};
};

} // namespace prismdrake::shell::controlclients
