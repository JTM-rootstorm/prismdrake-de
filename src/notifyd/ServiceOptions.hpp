#pragma once

#include "NotificationAuthority.hpp"
#include "Result.hpp"
#include "XdgPaths.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace prismdrake::notifyd {

enum class ServiceAction : std::uint8_t { run, show_help, show_version };

struct ServiceOptions final {
    ServiceAction action{ServiceAction::run};
    AuthorityOptions authority;
};

[[nodiscard]] foundation::Result<ServiceOptions>
parseServiceOptions(std::span<const std::string_view> arguments,
                    const foundation::XdgEnvironment &environment);

[[nodiscard]] std::string serviceHelpText();
[[nodiscard]] std::string serviceVersionText();

} // namespace prismdrake::notifyd
