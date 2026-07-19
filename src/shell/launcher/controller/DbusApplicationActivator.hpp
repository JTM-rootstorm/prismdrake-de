#pragma once

#include "ApplicationActivation.hpp"
#include "Cancellation.hpp"
#include "Result.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace prismdrake::shell::launcher::controller {

inline constexpr std::chrono::milliseconds applicationActivationTimeout{2000};

class LauncherApplicationActivator {
  public:
    virtual ~LauncherApplicationActivator() = default;

    /// Calls org.freedesktop.Application using only validated typed fields.
    [[nodiscard]] virtual foundation::Result<void>
    activate(const prismdrake::launcher::DesktopFileId &desktopFileId,
             std::optional<std::string_view> actionId,
             const foundation::CancellationToken &cancellation) = 0;
};

[[nodiscard]] std::unique_ptr<LauncherApplicationActivator>
makeDefaultLauncherApplicationActivator();

} // namespace prismdrake::shell::launcher::controller
