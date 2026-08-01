#pragma once

#include "MonotonicClock.hpp"
#include "Result.hpp"
#include "SdBusApi.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace prismdrake::shell::control {

enum class ShellAction : std::uint8_t {
    toggle_launcher,
    open_quick_settings,
    open_notification_center,
    focus_panel,
};

enum class ShellRequestFailure : std::uint8_t {
    none,
    invalid_output,
    output_unavailable,
    busy,
    timed_out,
    service_stopping,
    internal,
};

/// Narrow shell-presentation boundary. Implementations validate the requested
/// output against one coherent topology generation before committing a focus or
/// surface-routing operation through the WM-respecting shell focus router.
class ShellPresentationPort {
  public:
    virtual ~ShellPresentationPort() = default;
    [[nodiscard]] virtual ShellRequestFailure request(ShellAction action,
                                                      std::string_view outputName) = 0;
};

/// Experimental same-user Shell1 object. Name ownership remains with the
/// embedding shell runtime.
class ShellControlService final {
  public:
    explicit ShellControlService(ShellPresentationPort &presentationPort);
    ShellControlService(ShellPresentationPort &presentationPort,
                        std::shared_ptr<const foundation::MonotonicClock> clock,
                        foundation::MonotonicClock::Duration requestBudget = std::chrono::seconds{
                            1});

    [[nodiscard]] foundation::Result<void> attach(sd_bus *bus);
    [[nodiscard]] ShellRequestFailure request(ShellAction action, std::string_view outputName);
    void beginServiceStop() noexcept { service_stopping_ = true; }

  private:
    ShellPresentationPort *presentation_port_;
    std::shared_ptr<const foundation::MonotonicClock> clock_;
    foundation::MonotonicClock::Duration request_budget_;
    ipc::sdbus::Slot slot_;
    bool service_stopping_ = false;
};

} // namespace prismdrake::shell::control
