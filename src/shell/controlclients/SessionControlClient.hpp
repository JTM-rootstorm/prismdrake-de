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

/// Strong C++-only logout identity bound to one service-owner epoch.
struct LogoutTransactionId final {
    std::uint64_t ownerEpoch;
    std::uint64_t value;

    friend bool operator==(const LogoutTransactionId &, const LogoutTransactionId &) = default;
};

enum class SessionControlOperation : std::uint8_t { request, confirm, cancel };

enum class SessionState : std::uint8_t {
    unavailable,
    idle,
    confirmationRequested,
    confirmed,
    stoppingShell,
    stoppingNotifyd,
    stoppingSettingsd,
    complete,
    cancelled,
    failed,
};

struct SessionControlCompletion final {
    SessionControlOperation operation;
    quicksettings::CommandFailure failure;
    std::optional<LogoutTransactionId> transaction;
};

struct SessionStatePublication final {
    std::uint64_t ownerEpoch;
    SessionState state;
    std::optional<LogoutTransactionId> transaction;
};

/// Nonblocking, transaction-typed adapter for the Experimental Session1 contract.
///
/// This C++ boundary exposes no bus names, payloads, or transaction primitives to QML. Accepted
/// operations complete on a later Qt event-loop turn and are invalidated by any owner-epoch loss.
/// Construct, start, call, stop, and destroy the client on one Qt event-loop thread. Callback
/// functions must remain valid for the client lifetime; destruction suppresses queued callbacks.
class SessionControlClient final : public QObject, public quicksettings::LogoutControlPort {
    Q_OBJECT

  public:
    enum class State : std::uint8_t { stopped, connecting, unavailable, available, failed };
    Q_ENUM(State)

    using CompletionCallback = std::function<void(SessionControlCompletion)>;
    using StateCallback = std::function<void(SessionStatePublication)>;

    explicit SessionControlClient(CompletionCallback completionCallback,
                                  StateCallback stateCallback, QObject *parent = nullptr);
    ~SessionControlClient() override;

    SessionControlClient(const SessionControlClient &) = delete;
    SessionControlClient &operator=(const SessionControlClient &) = delete;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t ownerEpoch() const noexcept { return owner_epoch_; }
    [[nodiscard]] std::optional<LogoutTransactionId> currentTransaction() const noexcept {
        return current_transaction_;
    }

    [[nodiscard]] quicksettings::CommandResult requestLogout() override;
    [[nodiscard]] quicksettings::CommandResult confirmLogout(LogoutTransactionId transaction);
    [[nodiscard]] quicksettings::CommandResult cancelLogout(LogoutTransactionId transaction);

  signals:
    void stateChanged();

  private:
    struct PendingCommand final {
        SessionControlOperation operation;
        std::uint64_t ownerEpoch;
        std::string owner;
        std::optional<LogoutTransactionId> transaction;
    };

    static int matchInstalled(sd_bus_message *message, void *userdata,
                              sd_bus_error *returnError) noexcept;
    static int nameOwnerChanged(sd_bus_message *message, void *userdata,
                                sd_bus_error *returnError) noexcept;
    static int sessionStateChanged(sd_bus_message *message, void *userdata,
                                   sd_bus_error *returnError) noexcept;
    static int ownerReply(sd_bus_message *message, void *userdata,
                          sd_bus_error *returnError) noexcept;
    static int stateReply(sd_bus_message *message, void *userdata,
                          sd_bus_error *returnError) noexcept;
    static int commandReply(sd_bus_message *message, void *userdata,
                            sd_bus_error *returnError) noexcept;

    [[nodiscard]] foundation::Result<void> installMatches();
    [[nodiscard]] foundation::Result<void> queryOwner();
    [[nodiscard]] foundation::Result<void> querySessionState();
    [[nodiscard]] quicksettings::CommandResult
    dispatchCommand(SessionControlOperation operation,
                    std::optional<LogoutTransactionId> transaction);
    void handleBusFailure() noexcept;
    void applyOwner(std::string owner) noexcept;
    void invalidateOwner(quicksettings::CommandFailure failure, State state) noexcept;
    void finishPending(SessionControlCompletion completion) noexcept;
    void publishSessionState(std::uint64_t transactionValue, SessionState state) noexcept;
    void publishUnavailable() noexcept;
    void setState(State state) noexcept;
    [[nodiscard]] bool advanceOwnerEpoch() noexcept;

    CompletionCallback completion_callback_;
    StateCallback state_callback_;
    detail::QtSessionBusDispatcher dispatcher_;
    ipc::sdbus::Slot owner_match_;
    ipc::sdbus::Slot state_match_;
    ipc::sdbus::Slot owner_query_;
    ipc::sdbus::Slot state_query_;
    ipc::sdbus::Slot command_call_;
    std::optional<PendingCommand> pending_;
    std::optional<LogoutTransactionId> current_transaction_;
    State state_{State::stopped};
    SessionState session_state_{SessionState::unavailable};
    std::string owner_;
    std::string state_query_owner_;
    std::uint64_t owner_epoch_{0U};
    std::uint64_t state_query_epoch_{0U};
    std::uint8_t pending_match_installs_{0U};
    bool destroying_{false};
};

} // namespace prismdrake::shell::controlclients
