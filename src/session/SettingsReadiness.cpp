#include "SettingsReadiness.hpp"

#include "SdBusApi.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using ipc::sdbus::Bus;
using ipc::sdbus::Message;

constexpr char serviceName[] = "org.prismdrake.Settings1";
constexpr char objectPath[] = "/org/prismdrake/Settings1";
constexpr char snapshotInterface[] = "org.prismdrake.SettingsSnapshot1";
constexpr char snapshotMethod[] = "GetCurrentSnapshot";
constexpr std::uint32_t snapshotSchemaVersion = 1U;

class BusError final {
  public:
    BusError() = default;
    ~BusError() { sd_bus_error_free(&value); }

    BusError(const BusError &) = delete;
    BusError &operator=(const BusError &) = delete;

    sd_bus_error value{};
};

[[nodiscard]] Result<SettingsReadiness> unavailable() {
    return Result<SettingsReadiness>::failure(
        {ErrorCode::not_found, "The settings snapshot is not ready.",
         "Wait for the bounded settings readiness interval or inspect settingsd diagnostics."});
}

} // namespace

Result<SettingsReadiness> probeSettingsReadiness(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero() || timeout > maximumSettingsReadinessTimeout) {
        return Result<SettingsReadiness>::failure(
            {ErrorCode::invalid_argument, "The settings readiness timeout is invalid.",
             "Use a positive readiness timeout no greater than ten seconds."});
    }

    Bus bus;
    if (sd_bus_open_user(bus.put()) < 0) {
        return unavailable();
    }

    Message call;
    if (sd_bus_message_new_method_call(bus.get(), call.put(), serviceName, objectPath,
                                       snapshotInterface, snapshotMethod) < 0 ||
        sd_bus_message_append(call.get(), "u", snapshotSchemaVersion) < 0) {
        return Result<SettingsReadiness>::failure(
            {ErrorCode::io_error, "The settings readiness request could not be created.",
             "Restart the development session and inspect fixed component diagnostics."});
    }

    const auto timeoutMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    Message reply;
    BusError error;
    const int callResult =
        sd_bus_call(bus.get(), call.get(), static_cast<std::uint64_t>(timeoutMicros), &error.value,
                    reply.put());
    if (callResult < 0) {
        return unavailable();
    }

    std::uint64_t generation = 0U;
    const void *bytes = nullptr;
    std::size_t size = 0U;
    if (sd_bus_message_read(reply.get(), "t", &generation) < 0 ||
        sd_bus_message_read_array(reply.get(), SD_BUS_TYPE_BYTE, &bytes, &size) < 0 ||
        generation == 0U || size == 0U || size > maximumSettingsReadinessSnapshotBytes ||
        bytes == nullptr) {
        return Result<SettingsReadiness>::failure(
            {ErrorCode::validation_error, "The settings readiness reply is invalid.",
             "Restart settingsd and require one complete version-1 snapshot."});
    }

    return Result<SettingsReadiness>::success(SettingsReadiness{generation, size});
}

} // namespace prismdrake::session
