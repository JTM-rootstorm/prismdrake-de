#pragma once

#include "Result.hpp"
#include "SettingsEngine.hpp"

#include <csignal>
#include <cstdint>

namespace prismdrake::settingsd {

enum class ServiceEpochOutcome : std::uint8_t {
    stopped,
    disconnected,
};

/// Runs one well-known-name ownership epoch.
///
/// A fresh settings engine is built before the session bus name is requested.
/// Bus loss ends the epoch so the caller can rebuild generation one before
/// reacquiring the name. The signal flag must be owned by the process and may
/// be set by an async-signal-safe handler.
[[nodiscard]] foundation::Result<ServiceEpochOutcome>
runServiceEpoch(const settings::SettingsEngineOptions &options,
                const volatile std::sig_atomic_t &stopRequested);

} // namespace prismdrake::settingsd
