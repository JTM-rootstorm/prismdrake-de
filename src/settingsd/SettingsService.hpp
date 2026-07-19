#pragma once

#include "Result.hpp"
#include "ServiceWait.hpp"
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
/// reacquiring the name. The process must own both the signal flag and wake
/// descriptor; an async-signal-safe handler sets the flag and notifies the wake.
[[nodiscard]] foundation::Result<ServiceEpochOutcome>
runServiceEpoch(const settings::SettingsEngineOptions &options,
                const volatile std::sig_atomic_t &stopRequested, ServiceShutdownWake &shutdownWake);

} // namespace prismdrake::settingsd
