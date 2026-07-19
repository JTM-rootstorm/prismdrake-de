#include "PanelClock.hpp"

#include <QTime>

#include <utility>

namespace prismdrake::shell::panel {

foundation::Result<QString> formatPanelClock(const QDateTime &localDateTime, const QLocale &locale,
                                             config::ClockFormat format) {
    if (!localDateTime.isValid()) {
        return foundation::Result<QString>::failure(
            {foundation::ErrorCode::invalid_argument, "The panel clock input is invalid.",
             "Supply a valid local time and an explicit user locale."});
    }
    QString formatted;
    switch (format) {
    case config::ClockFormat::locale:
        formatted = locale.toString(localDateTime.time(), QLocale::ShortFormat);
        break;
    case config::ClockFormat::hour_12:
        formatted = locale.toString(localDateTime.time(), QStringLiteral("h:mm AP"));
        break;
    case config::ClockFormat::hour_24:
        formatted = locale.toString(localDateTime.time(), QStringLiteral("HH:mm"));
        break;
    default:
        return foundation::Result<QString>::failure(
            {foundation::ErrorCode::invalid_argument, "The panel clock format is invalid.",
             "Use locale, 12-hour, or 24-hour clock formatting."});
    }
    if (formatted.isEmpty()) {
        return foundation::Result<QString>::failure(
            {foundation::ErrorCode::validation_error, "The panel clock could not be formatted.",
             "Retain the prior clock presentation and retry at the next minute."});
    }
    return foundation::Result<QString>::success(std::move(formatted));
}

foundation::Result<std::chrono::milliseconds> nextPanelClockUpdate(const QDateTime &localDateTime) {
    if (!localDateTime.isValid()) {
        return foundation::Result<std::chrono::milliseconds>::failure(
            {foundation::ErrorCode::invalid_argument, "The panel clock input is invalid.",
             "Supply a valid local time."});
    }
    const QTime time = localDateTime.time();
    const int elapsed = time.second() * 1000 + time.msec();
    const int delay = 60'000 - elapsed;
    if (delay <= 0 || delay > 60'000) {
        return foundation::Result<std::chrono::milliseconds>::failure(
            {foundation::ErrorCode::validation_error, "The panel clock delay is invalid.",
             "Retry at the next bounded minute boundary."});
    }
    return foundation::Result<std::chrono::milliseconds>::success(std::chrono::milliseconds{delay});
}

} // namespace prismdrake::shell::panel
