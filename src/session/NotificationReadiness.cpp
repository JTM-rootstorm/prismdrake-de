#include "NotificationReadiness.hpp"

#include "SdBusApi.hpp"

#include <cerrno>
#include <cstdint>

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using ipc::sdbus::Bus;
using ipc::sdbus::Credentials;

constexpr char standardServiceName[] = "org.freedesktop.Notifications";
constexpr char centerServiceName[] = "org.prismdrake.NotificationCenter1";

[[nodiscard]] Result<void> unavailable() {
    return Result<void>::failure(
        {ErrorCode::not_found, "The notification service is not ready.",
         "Continue the bounded readiness wait for the exact notification child."});
}

[[nodiscard]] Result<void> verifyOwner(sd_bus *bus, const char *name, pid_t expectedProcess) {
    Credentials credentials;
    const int queried = sd_bus_get_name_creds(bus, name, SD_BUS_CREDS_PID, credentials.put());
    if (queried == -ESRCH || queried == -ENXIO || queried == -ENOENT) {
        return unavailable();
    }
    if (queried < 0) {
        return Result<void>::failure({ErrorCode::io_error,
                                      "The notification service owner could not be inspected.",
                                      "Retry within the bounded notification readiness interval."});
    }

    pid_t ownerProcess = -1;
    if (sd_bus_creds_get_pid(credentials.get(), &ownerProcess) < 0 || ownerProcess <= 0) {
        return Result<void>::failure(
            {ErrorCode::validation_error, "The notification service owner is invalid.",
             "Use a session bus that exposes process credentials for service owners."});
    }
    if (ownerProcess != expectedProcess) {
        return Result<void>::failure(
            {ErrorCode::unsupported, "A notification service name is already owned.",
             "Keep the existing owner and run Prismdrake with notifications degraded."});
    }
    return Result<void>::success();
}

} // namespace

Result<void> probeNotificationReadiness(pid_t expectedProcess, std::chrono::milliseconds timeout) {
    if (expectedProcess <= 0 || timeout <= std::chrono::milliseconds::zero() ||
        timeout > maximumNotificationReadinessTimeout) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "The notification readiness wait is invalid.",
             "Wait only for a positive exact child PID within the bounded deadline."});
    }

    Bus bus;
    if (sd_bus_open_user(bus.put()) < 0) {
        return unavailable();
    }
    const auto timeoutMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    if (sd_bus_set_method_call_timeout(bus.get(), static_cast<std::uint64_t>(timeoutMicros)) < 0) {
        return Result<void>::failure({ErrorCode::io_error,
                                      "The notification readiness timeout could not be applied.",
                                      "Retry with a functioning user session bus."});
    }

    auto standard = verifyOwner(bus.get(), standardServiceName, expectedProcess);
    if (!standard) {
        return standard;
    }
    return verifyOwner(bus.get(), centerServiceName, expectedProcess);
}

} // namespace prismdrake::session
