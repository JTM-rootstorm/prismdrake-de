#include "SessionControlClient.hpp"

#include <QTimer>

#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace prismdrake::shell::controlclients {
namespace {

using quicksettings::CommandFailure;
using quicksettings::CommandResult;

constexpr std::string_view serviceName = "org.prismdrake.Session1";
constexpr std::string_view objectPath = "/org/prismdrake/Session1";
constexpr std::string_view interfaceName = "org.prismdrake.Session1";
constexpr std::uint64_t getterTimeoutUsec = 2'000'000U;
constexpr std::uint64_t mutationTimeoutUsec = 6'000'000U;

[[nodiscard]] foundation::Error transportError() {
    return {foundation::ErrorCode::io_error, "The session control connection failed.",
            "Retry after the session service is available."};
}

[[nodiscard]] foundation::Error callbackError() {
    return {foundation::ErrorCode::invalid_argument,
            "The session control completion callback is missing.",
            "Supply a C++ completion callback before starting the client."};
}

[[nodiscard]] bool validUniqueOwner(std::string_view owner) noexcept {
    return owner.empty() || (owner.size() <= 255U && owner.front() == ':');
}

[[nodiscard]] bool isMethodError(sd_bus_message *message) noexcept {
    return message == nullptr || sd_bus_message_is_method_error(message, nullptr) > 0;
}

[[nodiscard]] CommandFailure methodFailure(sd_bus_message *message) noexcept {
    const auto *error = message == nullptr ? nullptr : sd_bus_message_get_error(message);
    const std::string_view name = error == nullptr || error->name == nullptr ? "" : error->name;
    if (name == "org.prismdrake.Session1.Error.Busy") {
        return CommandFailure::busy;
    }
    if (name == "org.prismdrake.Session1.Error.NotAuthorized") {
        return CommandFailure::denied;
    }
    if (name == "org.prismdrake.Session1.Error.ServiceStopping" ||
        name == "org.freedesktop.DBus.Error.NameHasNoOwner" ||
        name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
        name == "org.freedesktop.DBus.Error.Disconnected") {
        return CommandFailure::unavailable;
    }
    return CommandFailure::failed;
}

[[nodiscard]] std::optional<SessionState> parseState(std::string_view value) noexcept {
    constexpr std::array entries{
        std::pair{std::string_view{"idle"}, SessionState::idle},
        std::pair{std::string_view{"confirmation_requested"}, SessionState::confirmationRequested},
        std::pair{std::string_view{"confirmed"}, SessionState::confirmed},
        std::pair{std::string_view{"stopping_shell"}, SessionState::stoppingShell},
        std::pair{std::string_view{"stopping_notifyd"}, SessionState::stoppingNotifyd},
        std::pair{std::string_view{"stopping_settingsd"}, SessionState::stoppingSettingsd},
        std::pair{std::string_view{"complete"}, SessionState::complete},
        std::pair{std::string_view{"cancelled"}, SessionState::cancelled},
        std::pair{std::string_view{"failed"}, SessionState::failed},
    };
    for (const auto &[name, state] : entries) {
        if (name == value) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] const char *methodName(SessionControlOperation operation) noexcept {
    switch (operation) {
    case SessionControlOperation::request:
        return "RequestLogout";
    case SessionControlOperation::confirm:
        return "ConfirmLogout";
    case SessionControlOperation::cancel:
        return "CancelLogout";
    }
    return nullptr;
}

[[nodiscard]] bool validSignalStateTransaction(SessionState state,
                                               std::uint64_t transaction) noexcept {
    return state == SessionState::idle || transaction != 0U;
}

[[nodiscard]] bool validGetterStateTransaction(SessionState state,
                                               std::uint64_t transaction) noexcept {
    return state == SessionState::idle ? transaction == 0U : transaction != 0U;
}

} // namespace

SessionControlClient::SessionControlClient(CompletionCallback completionCallback,
                                           StateCallback stateCallback, QObject *parent)
    : QObject(parent), completion_callback_(std::move(completionCallback)),
      state_callback_(std::move(stateCallback)),
      dispatcher_([this]() noexcept { handleBusFailure(); }) {}

SessionControlClient::~SessionControlClient() {
    destroying_ = true;
    stop();
}

foundation::Result<void> SessionControlClient::start() {
    if (state_ != State::stopped) {
        return foundation::Result<void>::failure({foundation::ErrorCode::invalid_argument,
                                                  "The session client is already running.",
                                                  "Stop the client before starting it again."});
    }
    if (!completion_callback_) {
        return foundation::Result<void>::failure(callbackError());
    }
    auto opened = dispatcher_.start();
    if (!opened) {
        return opened;
    }
    setState(State::connecting);
    auto installed = installMatches();
    if (!installed) {
        const auto error = installed.error();
        stop();
        return foundation::Result<void>::failure(error);
    }
    dispatcher_.refresh();
    return foundation::Result<void>::success();
}

void SessionControlClient::stop() noexcept {
    owner_match_.reset();
    state_match_.reset();
    owner_query_.reset();
    state_query_.reset();
    command_call_.reset();
    pending_match_installs_ = 0U;
    owner_.clear();
    state_query_owner_.clear();
    current_transaction_.reset();
    session_state_ = SessionState::unavailable;
    if (pending_) {
        finishPending({pending_->operation, CommandFailure::unavailable, pending_->transaction});
    }
    publishUnavailable();
    dispatcher_.stop();
    setState(State::stopped);
}

foundation::Result<void> SessionControlClient::installMatches() {
    constexpr auto ownerRule =
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0='org.prismdrake.Session1'";
    constexpr auto stateRule = "type='signal',path='/org/prismdrake/Session1',"
                               "interface='org.prismdrake.Session1',member='SessionStateChanged'";
    if (sd_bus_add_match_async(dispatcher_.get(), owner_match_.put(), ownerRule, &nameOwnerChanged,
                               &matchInstalled, this) < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    ++pending_match_installs_;
    if (sd_bus_add_match_async(dispatcher_.get(), state_match_.put(), stateRule,
                               &sessionStateChanged, &matchInstalled, this) < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    ++pending_match_installs_;
    return foundation::Result<void>::success();
}

foundation::Result<void> SessionControlClient::queryOwner() {
    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(dispatcher_.get(), request.put(), "org.freedesktop.DBus",
                                       "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                       "GetNameOwner") < 0 ||
        sd_bus_message_append(request.get(), "s", serviceName.data()) < 0 ||
        sd_bus_call_async(dispatcher_.get(), owner_query_.put(), request.get(), &ownerReply, this,
                          getterTimeoutUsec) < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    dispatcher_.refresh();
    return foundation::Result<void>::success();
}

foundation::Result<void> SessionControlClient::querySessionState() {
    if (owner_.empty()) {
        return foundation::Result<void>::failure(transportError());
    }
    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(dispatcher_.get(), request.put(), serviceName.data(),
                                       objectPath.data(), interfaceName.data(),
                                       "GetSessionState") < 0) {
        return foundation::Result<void>::failure(transportError());
    }
    state_query_owner_ = owner_;
    state_query_epoch_ = owner_epoch_;
    if (sd_bus_call_async(dispatcher_.get(), state_query_.put(), request.get(), &stateReply, this,
                          getterTimeoutUsec) < 0) {
        state_query_owner_.clear();
        state_query_epoch_ = 0U;
        return foundation::Result<void>::failure(transportError());
    }
    dispatcher_.refresh();
    return foundation::Result<void>::success();
}

CommandResult SessionControlClient::requestLogout() {
    return dispatchCommand(SessionControlOperation::request, std::nullopt);
}

CommandResult SessionControlClient::confirmLogout(LogoutTransactionId transaction) {
    return dispatchCommand(SessionControlOperation::confirm, transaction);
}

CommandResult SessionControlClient::cancelLogout(LogoutTransactionId transaction) {
    return dispatchCommand(SessionControlOperation::cancel, transaction);
}

CommandResult
SessionControlClient::dispatchCommand(SessionControlOperation operation,
                                      std::optional<LogoutTransactionId> transaction) {
    if (state_ != State::available || owner_.empty() || !dispatcher_) {
        return {CommandFailure::unavailable};
    }
    if (pending_) {
        return {CommandFailure::busy};
    }
    if (operation == SessionControlOperation::request) {
        if (transaction || session_state_ != SessionState::idle) {
            return {CommandFailure::busy};
        }
    } else if (!transaction || transaction->value == 0U ||
               transaction->ownerEpoch != owner_epoch_ || current_transaction_ != transaction ||
               session_state_ != SessionState::confirmationRequested) {
        return {CommandFailure::failed};
    }
    const auto *method = methodName(operation);
    if (method == nullptr) {
        return {CommandFailure::failed};
    }

    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(dispatcher_.get(), request.put(), serviceName.data(),
                                       objectPath.data(), interfaceName.data(), method) < 0 ||
        (transaction && sd_bus_message_append(request.get(), "t", transaction->value) < 0)) {
        return {CommandFailure::failed};
    }
    pending_ = PendingCommand{operation, owner_epoch_, owner_, transaction};
    if (sd_bus_call_async(dispatcher_.get(), command_call_.put(), request.get(), &commandReply,
                          this, mutationTimeoutUsec) < 0) {
        pending_.reset();
        return {CommandFailure::failed};
    }
    dispatcher_.refresh();
    return {CommandFailure::none};
}

int SessionControlClient::matchInstalled(sd_bus_message *message, void *userdata,
                                         sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        if (isMethodError(message)) {
            self->invalidateOwner(CommandFailure::unavailable, State::failed);
            return 0;
        }
        if (self->pending_match_installs_ > 0U) {
            --self->pending_match_installs_;
        }
        if (self->pending_match_installs_ == 0U) {
            auto queried = self->queryOwner();
            if (!queried) {
                self->invalidateOwner(CommandFailure::unavailable, State::failed);
            }
        }
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SessionControlClient::nameOwnerChanged(sd_bus_message *message, void *userdata,
                                           sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        const char *name = nullptr;
        const char *oldOwner = nullptr;
        const char *newOwner = nullptr;
        if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) <= 0 ||
            name == nullptr || std::string_view{name} != serviceName || oldOwner == nullptr ||
            newOwner == nullptr || !validUniqueOwner(oldOwner) || !validUniqueOwner(newOwner) ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->owner_query_.reset();
        self->applyOwner(newOwner);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SessionControlClient::sessionStateChanged(sd_bus_message *message, void *userdata,
                                              sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    try {
        const auto *sender = sd_bus_message_get_sender(message);
        std::uint64_t transaction = 0U;
        const char *stateValue = nullptr;
        if (sender == nullptr || self->owner_ != sender) {
            return 0;
        }
        if (sd_bus_message_read(message, "ts", &transaction, &stateValue) <= 0 ||
            stateValue == nullptr || sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        const auto parsed = parseState(stateValue);
        if (!parsed || !validSignalStateTransaction(*parsed, transaction)) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->state_query_.reset();
        self->state_query_owner_.clear();
        self->state_query_epoch_ = 0U;
        self->publishSessionState(transaction, *parsed);
        self->setState(State::available);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SessionControlClient::ownerReply(sd_bus_message *message, void *userdata,
                                     sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->owner_query_.reset();
    try {
        if (sd_bus_message_is_method_error(message, "org.freedesktop.DBus.Error.NameHasNoOwner") >
            0) {
            self->applyOwner({});
            return 0;
        }
        const char *owner = nullptr;
        if (isMethodError(message) || sd_bus_message_read(message, "s", &owner) <= 0 ||
            owner == nullptr || !validUniqueOwner(owner) || std::string_view{owner}.empty() ||
            sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->applyOwner(owner);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SessionControlClient::stateReply(sd_bus_message *message, void *userdata,
                                     sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr) {
        return 0;
    }
    self->state_query_.reset();
    const auto expectedOwner = std::exchange(self->state_query_owner_, {});
    const auto expectedEpoch = std::exchange(self->state_query_epoch_, 0U);
    try {
        const auto *sender = sd_bus_message_get_sender(message);
        std::uint64_t transaction = 0U;
        const char *stateValue = nullptr;
        if (isMethodError(message) || sender == nullptr || expectedOwner.empty() ||
            self->owner_ != expectedOwner || self->owner_epoch_ != expectedEpoch ||
            expectedOwner != sender ||
            sd_bus_message_read(message, "ts", &transaction, &stateValue) <= 0 ||
            stateValue == nullptr || sd_bus_message_at_end(message, 1) <= 0) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        const auto parsed = parseState(stateValue);
        if (!parsed || !validGetterStateTransaction(*parsed, transaction)) {
            self->invalidateOwner(CommandFailure::failed, State::failed);
            return 0;
        }
        self->publishSessionState(transaction, *parsed);
        self->setState(State::available);
    } catch (...) {
        self->invalidateOwner(CommandFailure::failed, State::failed);
    }
    return 0;
}

int SessionControlClient::commandReply(sd_bus_message *message, void *userdata,
                                       sd_bus_error *) noexcept {
    auto *self = static_cast<SessionControlClient *>(userdata);
    if (self == nullptr || !self->pending_) {
        return 0;
    }
    self->command_call_.reset();
    try {
        if (self->owner_ != self->pending_->owner ||
            self->owner_epoch_ != self->pending_->ownerEpoch) {
            self->finishPending({self->pending_->operation, CommandFailure::unavailable,
                                 self->pending_->transaction});
            return 0;
        }
        const auto *sender = message == nullptr ? nullptr : sd_bus_message_get_sender(message);
        if (isMethodError(message)) {
            const auto *error = message == nullptr ? nullptr : sd_bus_message_get_error(message);
            const std::string_view errorName =
                error == nullptr || error->name == nullptr ? "" : error->name;
            if (errorName.starts_with("org.prismdrake.Session1.Error.") &&
                (sender == nullptr || self->pending_->owner != sender)) {
                self->finishPending({self->pending_->operation, CommandFailure::failed,
                                     self->pending_->transaction});
                return 0;
            }
            self->finishPending(
                {self->pending_->operation, methodFailure(message), self->pending_->transaction});
            return 0;
        }
        if (sender == nullptr || self->pending_->owner != sender) {
            self->finishPending(
                {self->pending_->operation, CommandFailure::failed, self->pending_->transaction});
            return 0;
        }
        if (self->pending_->operation == SessionControlOperation::request) {
            std::uint64_t transactionValue = 0U;
            if (sd_bus_message_read(message, "t", &transactionValue) <= 0 ||
                transactionValue == 0U || sd_bus_message_at_end(message, 1) <= 0) {
                self->finishPending(
                    {self->pending_->operation, CommandFailure::failed, std::nullopt});
                return 0;
            }
            const LogoutTransactionId transaction{self->owner_epoch_, transactionValue};
            if (self->current_transaction_ && self->current_transaction_ != transaction) {
                self->finishPending(
                    {self->pending_->operation, CommandFailure::failed, std::nullopt});
                return 0;
            }
            self->current_transaction_ = transaction;
            self->finishPending(
                {SessionControlOperation::request, CommandFailure::none, transaction});
            return 0;
        }
        if (sd_bus_message_at_end(message, 1) <= 0) {
            self->finishPending(
                {self->pending_->operation, CommandFailure::failed, self->pending_->transaction});
            return 0;
        }
        self->finishPending(
            {self->pending_->operation, CommandFailure::none, self->pending_->transaction});
    } catch (...) {
        self->finishPending(
            {self->pending_->operation, CommandFailure::failed, self->pending_->transaction});
    }
    return 0;
}

void SessionControlClient::handleBusFailure() noexcept {
    owner_match_.reset();
    state_match_.reset();
    owner_query_.reset();
    state_query_.reset();
    command_call_.reset();
    pending_match_installs_ = 0U;
    invalidateOwner(CommandFailure::unavailable, State::failed);
}

void SessionControlClient::applyOwner(std::string owner) noexcept {
    if (!validUniqueOwner(owner)) {
        invalidateOwner(CommandFailure::failed, State::failed);
        return;
    }
    if (owner_ == owner) {
        if (owner.empty()) {
            publishUnavailable();
            setState(State::unavailable);
        }
        return;
    }
    if (!advanceOwnerEpoch()) {
        invalidateOwner(CommandFailure::failed, State::failed);
        return;
    }
    state_query_.reset();
    command_call_.reset();
    state_query_owner_.clear();
    state_query_epoch_ = 0U;
    current_transaction_.reset();
    session_state_ = SessionState::unavailable;
    if (pending_) {
        finishPending({pending_->operation, CommandFailure::unavailable, pending_->transaction});
    }
    owner_ = std::move(owner);
    if (owner_.empty()) {
        publishUnavailable();
        setState(State::unavailable);
        return;
    }
    setState(State::connecting);
    auto queried = querySessionState();
    if (!queried) {
        invalidateOwner(CommandFailure::unavailable, State::failed);
    }
}

void SessionControlClient::invalidateOwner(CommandFailure failure, State state) noexcept {
    state_query_.reset();
    command_call_.reset();
    owner_query_.reset();
    state_query_owner_.clear();
    state_query_epoch_ = 0U;
    (void)advanceOwnerEpoch();
    owner_.clear();
    current_transaction_.reset();
    session_state_ = SessionState::unavailable;
    if (pending_) {
        finishPending({pending_->operation, failure, pending_->transaction});
    }
    publishUnavailable();
    setState(state);
}

void SessionControlClient::finishPending(SessionControlCompletion completion) noexcept {
    if (!pending_) {
        return;
    }
    pending_.reset();
    if (destroying_) {
        return;
    }
    QTimer::singleShot(0, this, [this, completion = std::move(completion)]() mutable {
        try {
            completion_callback_(std::move(completion));
        } catch (...) {
        }
    });
}

void SessionControlClient::publishSessionState(std::uint64_t transactionValue,
                                               SessionState state) noexcept {
    std::optional<LogoutTransactionId> transaction;
    if (transactionValue != 0U) {
        transaction = LogoutTransactionId{owner_epoch_, transactionValue};
    }
    session_state_ = state;
    if (state == SessionState::idle || state == SessionState::complete ||
        state == SessionState::cancelled || state == SessionState::failed) {
        current_transaction_.reset();
    } else {
        current_transaction_ = transaction;
    }
    if (destroying_ || !state_callback_) {
        return;
    }
    const SessionStatePublication publication{owner_epoch_, state, transaction};
    QTimer::singleShot(0, this, [this, publication]() {
        try {
            state_callback_(publication);
        } catch (...) {
        }
    });
}

void SessionControlClient::publishUnavailable() noexcept {
    session_state_ = SessionState::unavailable;
    current_transaction_.reset();
    if (destroying_ || !state_callback_) {
        return;
    }
    const SessionStatePublication publication{owner_epoch_, SessionState::unavailable,
                                              std::nullopt};
    QTimer::singleShot(0, this, [this, publication]() {
        try {
            state_callback_(publication);
        } catch (...) {
        }
    });
}

void SessionControlClient::setState(State state) noexcept {
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged();
}

bool SessionControlClient::advanceOwnerEpoch() noexcept {
    if (owner_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++owner_epoch_;
    return true;
}

} // namespace prismdrake::shell::controlclients
