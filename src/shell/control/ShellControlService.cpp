#include "ShellControlService.hpp"

#include "PeerCredentials.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace prismdrake::shell::control {
namespace {

constexpr char objectPath[] = "/org/prismdrake/Shell1";
constexpr char interfaceName[] = "org.prismdrake.Shell1";
constexpr std::size_t maximumOutputNameBytes = 255U;

constexpr char invalidOutputError[] = "org.prismdrake.Shell1.Error.InvalidOutput";
constexpr char outputUnavailableError[] = "org.prismdrake.Shell1.Error.OutputUnavailable";
constexpr char busyError[] = "org.prismdrake.Shell1.Error.Busy";
constexpr char timedOutError[] = "org.prismdrake.Shell1.Error.TimedOut";
constexpr char notAuthorizedError[] = "org.prismdrake.Shell1.Error.NotAuthorized";
constexpr char serviceStoppingError[] = "org.prismdrake.Shell1.Error.ServiceStopping";
constexpr char internalError[] = "org.prismdrake.Shell1.Error.Internal";

constexpr char invalidOutputMessage[] = "The requested output name is invalid.";
constexpr char outputUnavailableMessage[] = "The requested output is not available.";
constexpr char busyMessage[] = "The shell cannot accept that presentation request now.";
constexpr char timedOutMessage[] = "The shell request exceeded its bounded service deadline.";
constexpr char notAuthorizedMessage[] = "The shell request is not authorized.";
constexpr char serviceStoppingMessage[] = "The shell service is stopping.";
constexpr char internalMessage[] = "The shell service could not complete the request.";

struct ErrorReply final {
    const char *name;
    const char *message;
};

[[nodiscard]] ErrorReply mapFailure(ShellRequestFailure failure) noexcept {
    switch (failure) {
    case ShellRequestFailure::none:
        break;
    case ShellRequestFailure::invalid_output:
        return {invalidOutputError, invalidOutputMessage};
    case ShellRequestFailure::output_unavailable:
        return {outputUnavailableError, outputUnavailableMessage};
    case ShellRequestFailure::busy:
        return {busyError, busyMessage};
    case ShellRequestFailure::timed_out:
        return {timedOutError, timedOutMessage};
    case ShellRequestFailure::service_stopping:
        return {serviceStoppingError, serviceStoppingMessage};
    case ShellRequestFailure::internal:
        return {internalError, internalMessage};
    }
    return {internalError, internalMessage};
}

[[nodiscard]] bool validUtf8OutputName(std::string_view value) noexcept {
    if (value.size() > maximumOutputNameBytes) {
        return false;
    }
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<std::uint8_t>(value[index]);
        if (lead < 0x80U) {
            if (lead < 0x20U || lead == 0x7fU) {
                return false;
            }
            ++index;
            continue;
        }
        std::size_t continuationCount = 0;
        std::uint32_t codepoint = 0;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuationCount = 1;
            codepoint = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuationCount = 2;
            codepoint = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuationCount = 3;
            codepoint = lead & 0x07U;
        } else {
            return false;
        }
        if (index + continuationCount >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto byte = static_cast<std::uint8_t>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        const bool overlong = (continuationCount == 2 && codepoint < 0x800U) ||
                              (continuationCount == 3 && codepoint < 0x10000U);
        if (overlong || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            (codepoint >= 0x80U && codepoint <= 0x9fU)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

[[nodiscard]] foundation::Error attachError() {
    return {foundation::ErrorCode::io_error,
            "Could not register the Experimental shell-control interface.",
            "Reconnect to the session bus and begin a fresh owner epoch."};
}

[[nodiscard]] ShellControlService &fromUserData(void *userData) {
    return *static_cast<ShellControlService *>(userData);
}

[[nodiscard]] int handleRequest(sd_bus_message *message, void *userData, sd_bus_error *error,
                                ShellAction action) {
    if (!ipc::isSameUserPeer(message)) {
        return sd_bus_error_set_const(error, notAuthorizedError, notAuthorizedMessage);
    }
    const char *outputName = nullptr;
    if (sd_bus_message_read(message, "s", &outputName) < 0 || outputName == nullptr) {
        return -EINVAL;
    }
    const auto failure = fromUserData(userData).request(action, outputName);
    if (failure != ShellRequestFailure::none) {
        const auto mapped = mapFailure(failure);
        return sd_bus_error_set_const(error, mapped.name, mapped.message);
    }
    return sd_bus_reply_method_return(message, "");
}

[[nodiscard]] int handleToggleLauncher(sd_bus_message *message, void *userData,
                                       sd_bus_error *error) {
    return handleRequest(message, userData, error, ShellAction::toggle_launcher);
}

[[nodiscard]] int handleOpenQuickSettings(sd_bus_message *message, void *userData,
                                          sd_bus_error *error) {
    return handleRequest(message, userData, error, ShellAction::open_quick_settings);
}

[[nodiscard]] int handleOpenNotificationCenter(sd_bus_message *message, void *userData,
                                               sd_bus_error *error) {
    return handleRequest(message, userData, error, ShellAction::open_notification_center);
}

[[nodiscard]] int handleFocusPanel(sd_bus_message *message, void *userData, sd_bus_error *error) {
    return handleRequest(message, userData, error, ShellAction::focus_panel);
}

[[nodiscard]] const sd_bus_vtable *shellVtable() {
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("ToggleLauncher", "s", "", handleToggleLauncher, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("OpenQuickSettings", "s", "", handleOpenQuickSettings,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("OpenNotificationCenter", "s", "", handleOpenNotificationCenter,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("FocusPanel", "s", "", handleFocusPanel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
    };
    return vtable;
}

} // namespace

ShellControlService::ShellControlService(ShellPresentationPort &presentationPort)
    : ShellControlService(presentationPort, std::make_shared<foundation::SystemMonotonicClock>()) {}

ShellControlService::ShellControlService(ShellPresentationPort &presentationPort,
                                         std::shared_ptr<const foundation::MonotonicClock> clock,
                                         foundation::MonotonicClock::Duration requestBudget)
    : presentation_port_(&presentationPort),
      clock_(clock ? std::move(clock) : std::make_shared<foundation::SystemMonotonicClock>()),
      request_budget_(requestBudget) {}

foundation::Result<void> ShellControlService::attach(sd_bus *bus) {
    if (bus == nullptr || slot_) {
        return foundation::Result<void>::failure(attachError());
    }
    if (sd_bus_add_object_vtable(bus, slot_.put(), objectPath, interfaceName, shellVtable(), this) <
        0) {
        return foundation::Result<void>::failure(attachError());
    }
    return foundation::Result<void>::success();
}

ShellRequestFailure ShellControlService::request(ShellAction action, std::string_view outputName) {
    const auto startedAt = clock_->now();
    if (request_budget_ <= foundation::MonotonicClock::Duration::zero() ||
        startedAt > foundation::MonotonicClock::TimePoint::max() - request_budget_) {
        return ShellRequestFailure::timed_out;
    }
    const auto deadline = startedAt + request_budget_;
    if (service_stopping_) {
        return ShellRequestFailure::service_stopping;
    }
    if (!outputName.empty() && !validUtf8OutputName(outputName)) {
        return ShellRequestFailure::invalid_output;
    }
    return presentation_port_->request(action, outputName, deadline);
}

} // namespace prismdrake::shell::control
