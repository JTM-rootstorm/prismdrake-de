#include "DbusApplicationActivator.hpp"

#include "SdBusApi.hpp"

#include <chrono>
#include <cstdint>

namespace prismdrake::shell::launcher::controller {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using ipc::sdbus::Bus;
using ipc::sdbus::Message;

constexpr char applicationInterface[] = "org.freedesktop.Application";

class BusError final {
  public:
    BusError() = default;
    ~BusError() { sd_bus_error_free(&value); }

    BusError(const BusError &) = delete;
    BusError &operator=(const BusError &) = delete;

    sd_bus_error value{};
};

[[nodiscard]] Result<void> unavailable() {
    return Result<void>::failure(
        {ErrorCode::not_found, "D-Bus application activation was unavailable.",
         "Retry activation or use the desktop-entry execution fallback when permitted."});
}

[[nodiscard]] bool appendEmptyPlatformData(sd_bus_message *message) {
    return sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") >= 0 &&
           sd_bus_message_close_container(message) >= 0;
}

class SdBusApplicationActivator final : public LauncherApplicationActivator {
  public:
    Result<void> activate(const prismdrake::launcher::DesktopFileId &desktopFileId,
                          std::optional<std::string_view> actionId,
                          const foundation::CancellationToken &cancellation) override {
        if (cancellation.isCancellationRequested()) {
            return Result<void>::failure({ErrorCode::cancelled,
                                          "D-Bus application activation was cancelled.",
                                          "Discard the stale activation request."});
        }
        auto identity = prismdrake::launcher::deriveDbusApplicationIdentity(desktopFileId);
        if (!identity) {
            return Result<void>::failure(identity.error());
        }
        auto action = prismdrake::launcher::validateDbusDesktopAction(actionId);
        if (!action) {
            return Result<void>::failure(action.error());
        }

        Bus bus;
        if (sd_bus_open_user(bus.put()) < 0) {
            return unavailable();
        }
        const char *method = action.value() ? "ActivateAction" : "Activate";
        Message call;
        if (sd_bus_message_new_method_call(
                bus.get(), call.put(), identity.value().serviceName.c_str(),
                identity.value().objectPath.c_str(), applicationInterface, method) < 0) {
            return unavailable();
        }
        if (action.value()) {
            if (sd_bus_message_append(call.get(), "s", action.value()->c_str()) < 0 ||
                sd_bus_message_open_container(call.get(), SD_BUS_TYPE_ARRAY, "v") < 0 ||
                sd_bus_message_close_container(call.get()) < 0) {
                return unavailable();
            }
        }
        if (!appendEmptyPlatformData(call.get())) {
            return unavailable();
        }

        const auto timeout =
            std::chrono::duration_cast<std::chrono::microseconds>(applicationActivationTimeout)
                .count();
        Message reply;
        BusError error;
        if (sd_bus_call(bus.get(), call.get(), static_cast<std::uint64_t>(timeout), &error.value,
                        reply.put()) < 0) {
            return unavailable();
        }
        return Result<void>::success();
    }
};

} // namespace

std::unique_ptr<LauncherApplicationActivator> makeDefaultLauncherApplicationActivator() {
    return std::make_unique<SdBusApplicationActivator>();
}

} // namespace prismdrake::shell::launcher::controller
