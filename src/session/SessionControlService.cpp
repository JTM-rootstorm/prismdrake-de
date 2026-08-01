#include "SessionControlService.hpp"

#include "PeerCredentials.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <sys/random.h>
#include <unistd.h>

namespace prismdrake::session {
namespace {

constexpr char objectPath[] = "/org/prismdrake/Session1";
constexpr char interfaceName[] = "org.prismdrake.Session1";

constexpr char noTransactionError[] = "org.prismdrake.Session1.Error.NoTransaction";
constexpr char transactionMismatchError[] = "org.prismdrake.Session1.Error.TransactionMismatch";
constexpr char invalidStateError[] = "org.prismdrake.Session1.Error.InvalidState";
constexpr char busyError[] = "org.prismdrake.Session1.Error.Busy";
constexpr char timedOutError[] = "org.prismdrake.Session1.Error.TimedOut";
constexpr char notAuthorizedError[] = "org.prismdrake.Session1.Error.NotAuthorized";
constexpr char serviceStoppingError[] = "org.prismdrake.Session1.Error.ServiceStopping";
constexpr char internalError[] = "org.prismdrake.Session1.Error.Internal";

constexpr char noTransactionMessage[] = "No logout transaction is active.";
constexpr char transactionMismatchMessage[] = "The logout transaction is stale or unknown.";
constexpr char invalidStateMessage[] = "The logout transaction is not in the required state.";
constexpr char busyMessage[] = "A logout transaction is already active.";
constexpr char timedOutMessage[] = "The session request exceeded its bounded service deadline.";
constexpr char notAuthorizedMessage[] = "The session request is not authorized.";
constexpr char serviceStoppingMessage[] = "The session service is stopping.";
constexpr char internalMessage[] = "The session service could not complete the request.";

struct ErrorReply final {
    const char *name;
    const char *message;
};

[[nodiscard]] ErrorReply mapFailure(LogoutRequestFailure failure) noexcept {
    switch (failure) {
    case LogoutRequestFailure::none:
        break;
    case LogoutRequestFailure::no_transaction:
        return {noTransactionError, noTransactionMessage};
    case LogoutRequestFailure::transaction_mismatch:
        return {transactionMismatchError, transactionMismatchMessage};
    case LogoutRequestFailure::invalid_state:
        return {invalidStateError, invalidStateMessage};
    case LogoutRequestFailure::busy:
        return {busyError, busyMessage};
    case LogoutRequestFailure::timed_out:
        return {timedOutError, timedOutMessage};
    case LogoutRequestFailure::service_stopping:
        return {serviceStoppingError, serviceStoppingMessage};
    case LogoutRequestFailure::internal:
        return {internalError, internalMessage};
    }
    return {internalError, internalMessage};
}

[[nodiscard]] foundation::Error attachError() {
    return {foundation::ErrorCode::io_error,
            "Could not register the Experimental session-control interface.",
            "Reconnect to the session bus and begin a fresh owner epoch."};
}

[[nodiscard]] std::uint64_t firstTransactionIdForOwnerEpoch() noexcept {
    std::uint64_t value = 0;
    auto *bytes = reinterpret_cast<unsigned char *>(&value);
    std::size_t received = 0;
    while (received < sizeof(value)) {
        const auto result = ::getrandom(bytes + received, sizeof(value) - received, GRND_NONBLOCK);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (received == sizeof(value) && value != 0) {
        return value;
    }

    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    value = static_cast<std::uint64_t>(ticks) ^ (static_cast<std::uint64_t>(::getpid()) << 32U);
    return value == 0 ? 1 : value;
}

[[nodiscard]] SessionControlService &fromUserData(void *userData) {
    return *static_cast<SessionControlService *>(userData);
}

[[nodiscard]] int authorize(sd_bus_message *message, sd_bus_error *error) {
    if (!ipc::isSameUserPeer(message)) {
        return sd_bus_error_set_const(error, notAuthorizedError, notAuthorizedMessage);
    }
    return 0;
}

[[nodiscard]] int replyFailure(sd_bus_error *error, LogoutRequestFailure failure) {
    const auto mapped = mapFailure(failure);
    return sd_bus_error_set_const(error, mapped.name, mapped.message);
}

[[nodiscard]] int handleGetSessionState(sd_bus_message *message, void *userData,
                                        sd_bus_error *error) {
    const int authorized = authorize(message, error);
    if (authorized < 0) {
        return authorized;
    }
    const auto result = fromUserData(userData).getSessionState();
    if (!result.accepted()) {
        return replyFailure(error, result.failure);
    }
    const std::string stateName{logoutStateName(result.snapshot.state)};
    return sd_bus_reply_method_return(message, "ts", result.snapshot.transactionId,
                                      stateName.c_str());
}

[[nodiscard]] int handleRequestLogout(sd_bus_message *message, void *userData,
                                      sd_bus_error *error) {
    const int authorized = authorize(message, error);
    if (authorized < 0) {
        return authorized;
    }
    auto &service = fromUserData(userData);
    const auto result = service.requestLogout();
    if (!result.accepted()) {
        return replyFailure(error, result.failure);
    }
    return sd_bus_reply_method_return(message, "t", result.transactionId);
}

enum class TransactionMutation : std::uint8_t { confirm, cancel };

template <TransactionMutation Operation>
[[nodiscard]] int handleTransactionMutation(sd_bus_message *message, void *userData,
                                            sd_bus_error *error) {
    const int authorized = authorize(message, error);
    if (authorized < 0) {
        return authorized;
    }
    std::uint64_t transactionId = 0;
    if (sd_bus_message_read(message, "t", &transactionId) < 0) {
        return -EINVAL;
    }
    auto &service = fromUserData(userData);
    const auto result = Operation == TransactionMutation::confirm
                            ? service.confirmLogout(transactionId)
                            : service.cancelLogout(transactionId);
    if (!result.accepted()) {
        return replyFailure(error, result.failure);
    }
    return sd_bus_reply_method_return(message, "");
}

[[nodiscard]] int handleConfirmLogout(sd_bus_message *message, void *userData,
                                      sd_bus_error *error) {
    return handleTransactionMutation<TransactionMutation::confirm>(message, userData, error);
}

[[nodiscard]] int handleCancelLogout(sd_bus_message *message, void *userData, sd_bus_error *error) {
    return handleTransactionMutation<TransactionMutation::cancel>(message, userData, error);
}

[[nodiscard]] const sd_bus_vtable *sessionVtable() {
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("GetSessionState", "", "ts", handleGetSessionState,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestLogout", "", "t", handleRequestLogout, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ConfirmLogout", "t", "", handleConfirmLogout, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("CancelLogout", "t", "", handleCancelLogout, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL("SessionStateChanged", "ts", 0),
        SD_BUS_VTABLE_END,
    };
    return vtable;
}

} // namespace

SessionControlService::SessionControlService(ExactChildShutdownPort &shutdownPort)
    : SessionControlService(shutdownPort, std::make_shared<foundation::SystemMonotonicClock>()) {}

SessionControlService::SessionControlService(
    ExactChildShutdownPort &shutdownPort, std::shared_ptr<const foundation::MonotonicClock> clock,
    foundation::MonotonicClock::Duration getterBudget,
    foundation::MonotonicClock::Duration mutationBudget)
    : clock_(clock ? std::move(clock) : std::make_shared<foundation::SystemMonotonicClock>()),
      getter_budget_(getterBudget),
      transaction_(
          shutdownPort, [this](const LogoutSnapshot &state) { emitTransition(state); }, clock_,
          mutationBudget, firstTransactionIdForOwnerEpoch()) {}

foundation::Result<void> SessionControlService::attach(sd_bus *bus) {
    if (bus == nullptr || bus_ != nullptr) {
        return foundation::Result<void>::failure(attachError());
    }
    const int result = sd_bus_add_object_vtable(bus, slot_.put(), objectPath, interfaceName,
                                                sessionVtable(), this);
    if (result < 0) {
        return foundation::Result<void>::failure(attachError());
    }
    bus_ = bus;
    return foundation::Result<void>::success();
}

LogoutSnapshot SessionControlService::snapshot() const noexcept { return transaction_.snapshot(); }

SessionStateResult SessionControlService::getSessionState() const {
    const auto startedAt = clock_->now();
    if (getter_budget_ <= foundation::MonotonicClock::Duration::zero() ||
        startedAt > foundation::MonotonicClock::TimePoint::max() - getter_budget_) {
        return {LogoutRequestFailure::timed_out, {}};
    }
    const auto deadline = startedAt + getter_budget_;
    const auto state = transaction_.snapshot();
    if (clock_->now() >= deadline) {
        return {LogoutRequestFailure::timed_out, {}};
    }
    return {LogoutRequestFailure::none, state};
}

LogoutRequestResult SessionControlService::requestLogout() { return transaction_.request(); }

LogoutRequestResult SessionControlService::confirmLogout(std::uint64_t transactionId) {
    return transaction_.confirm(transactionId);
}

LogoutRequestResult SessionControlService::cancelLogout(std::uint64_t transactionId) {
    return transaction_.cancel(transactionId);
}

LogoutRequestResult SessionControlService::observeShutdownEvent(std::uint64_t transactionId,
                                                                ExactChildShutdownEvent event) {
    return transaction_.observeShutdownEvent(transactionId, event);
}

void SessionControlService::beginServiceStop() noexcept { transaction_.beginServiceStop(); }

void SessionControlService::emitTransition(const LogoutSnapshot &state) noexcept {
    if (bus_ == nullptr) {
        return;
    }
    const std::string stateName{logoutStateName(state.state)};
    (void)sd_bus_emit_signal(bus_, objectPath, interfaceName, "SessionStateChanged", "ts",
                             state.transactionId, stateName.c_str());
}

} // namespace prismdrake::session
