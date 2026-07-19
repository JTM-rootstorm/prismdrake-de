#pragma once

#include "Configuration.hpp"
#include "Result.hpp"

#include <QDateTime>
#include <QLocale>
#include <QString>

#include <chrono>

namespace prismdrake::shell::panel {

/// Formats one injected local date/time without consulting process-global locale state.
[[nodiscard]] foundation::Result<QString>
formatPanelClock(const QDateTime &localDateTime, const QLocale &locale, config::ClockFormat format);

/// Returns the bounded delay to the next local minute boundary.
[[nodiscard]] foundation::Result<std::chrono::milliseconds>
nextPanelClockUpdate(const QDateTime &localDateTime);

} // namespace prismdrake::shell::panel
