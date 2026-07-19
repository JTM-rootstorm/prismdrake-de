#include "ShellCtl.hpp"

#include "SdBusApi.hpp"

#include <cerrno>
#include <cstring>

namespace prismdrake::shellctl {
namespace {

constexpr char shellService[] = "org.prismdrake.Shell1";
constexpr char shellPath[] = "/org/prismdrake/Shell1";
constexpr char shellInterface[] = "org.prismdrake.Shell1";
constexpr char sessionService[] = "org.prismdrake.Session1";
constexpr char sessionPath[] = "/org/prismdrake/Session1";
constexpr char sessionInterface[] = "org.prismdrake.Session1";
constexpr std::uint64_t clientTimeoutMicroseconds = 2'000'000U;

struct Call final {
    const char *service;
    const char *path;
    const char *interface;
    const char *method;
    bool takesOutput;
};

[[nodiscard]] Call callFor(Command command) noexcept {
    switch (command) {
    case Command::launcher_toggle:
        return {shellService, shellPath, shellInterface, "ToggleLauncher", true};
    case Command::quick_settings_open:
        return {shellService, shellPath, shellInterface, "OpenQuickSettings", true};
    case Command::notifications_open:
        return {shellService, shellPath, shellInterface, "OpenNotificationCenter", true};
    case Command::panel_focus:
        return {shellService, shellPath, shellInterface, "FocusPanel", true};
    case Command::session_logout:
        return {sessionService, sessionPath, sessionInterface, "RequestLogout", false};
    }
    return {shellService, shellPath, shellInterface, "", false};
}

[[nodiscard]] bool errorHasPrefix(const sd_bus_error &error, const char *prefix) noexcept {
    return error.name != nullptr && std::strncmp(error.name, prefix, std::strlen(prefix)) == 0;
}

[[nodiscard]] ExitStatus mapCallFailure(int result, const sd_bus_error &error) noexcept {
    if (result == -ETIMEDOUT || errorHasPrefix(error, "org.prismdrake.Shell1.Error.TimedOut") ||
        errorHasPrefix(error, "org.prismdrake.Session1.Error.TimedOut")) {
        return ExitStatus::timeout;
    }
    if (errorHasPrefix(error, "org.prismdrake.Shell1.Error.NotAuthorized") ||
        errorHasPrefix(error, "org.prismdrake.Session1.Error.NotAuthorized") || result == -EACCES ||
        result == -EPERM) {
        return ExitStatus::authorization_failure;
    }
    if (errorHasPrefix(error, "org.freedesktop.DBus.Error.ServiceUnknown") ||
        errorHasPrefix(error, "org.freedesktop.DBus.Error.NameHasNoOwner") || result == -ENXIO ||
        result == -ECONNREFUSED || result == -ENOTCONN) {
        return ExitStatus::unavailable_service;
    }
    if (errorHasPrefix(error, "org.prismdrake.Shell1.Error.") ||
        errorHasPrefix(error, "org.prismdrake.Session1.Error.")) {
        return ExitStatus::rejected_state;
    }
    return ExitStatus::internal_failure;
}

} // namespace

ParsedCommand parseCommand(std::span<const std::string_view> arguments) noexcept {
    if (arguments.size() != 2U) {
        return {.failure = ParseFailure::invalid_usage};
    }
    const auto group = arguments[0];
    const auto action = arguments[1];
    if (group == "launcher" && action == "toggle") {
        return {.command = Command::launcher_toggle};
    }
    if (group == "quick-settings" && action == "open") {
        return {.command = Command::quick_settings_open};
    }
    if (group == "notifications" && action == "open") {
        return {.command = Command::notifications_open};
    }
    if (group == "panel" && action == "focus") {
        return {.command = Command::panel_focus};
    }
    if (group == "session" && action == "logout") {
        return {.command = Command::session_logout};
    }
    return {.failure = ParseFailure::invalid_usage};
}

std::string_view usageText() noexcept {
    return "Usage:\n"
           "  prismdrake-shellctl launcher toggle\n"
           "  prismdrake-shellctl quick-settings open\n"
           "  prismdrake-shellctl notifications open\n"
           "  prismdrake-shellctl panel focus\n"
           "  prismdrake-shellctl session logout\n";
}

ExitStatus invoke(Command command) noexcept {
    ipc::sdbus::Bus bus;
    if (sd_bus_open_user(bus.put()) < 0) {
        return ExitStatus::unavailable_service;
    }

    const auto call = callFor(command);
    ipc::sdbus::Message request;
    if (sd_bus_message_new_method_call(bus.get(), request.put(), call.service, call.path,
                                       call.interface, call.method) < 0) {
        return ExitStatus::internal_failure;
    }
    if (call.takesOutput && sd_bus_message_append(request.get(), "s", "") < 0) {
        return ExitStatus::internal_failure;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    ipc::sdbus::Message reply;
    const int result =
        sd_bus_call(bus.get(), request.get(), clientTimeoutMicroseconds, &error, reply.put());
    const auto status = result < 0 ? mapCallFailure(result, error) : ExitStatus::success;
    sd_bus_error_free(&error);
    return status;
}

} // namespace prismdrake::shellctl
