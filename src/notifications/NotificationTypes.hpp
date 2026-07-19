#pragma once

#include "MonotonicClock.hpp"
#include "Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::notifications {

inline constexpr std::size_t maximumServiceRecords = 256U;
inline constexpr std::size_t maximumServiceActions = 8U;
inline constexpr std::size_t maximumServiceApplicationNameBytes = 512U;
inline constexpr std::size_t maximumServiceApplicationNameCodepoints = 128U;
inline constexpr std::size_t maximumServiceSummaryBytes = 1024U;
inline constexpr std::size_t maximumServiceSummaryCodepoints = 256U;
inline constexpr std::size_t maximumServiceBodyBytes = 16U * 1024U;
inline constexpr std::size_t maximumServiceBodyCodepoints = 4096U;
inline constexpr std::size_t maximumServiceDesktopFileIdBytes = 16U * 1024U;
inline constexpr std::size_t maximumServiceDesktopFileIdCodepoints = 4096U;
inline constexpr std::size_t maximumServiceActionIdBytes = 256U;
inline constexpr std::size_t maximumServiceActionIdCodepoints = 64U;
inline constexpr std::size_t maximumServiceActionLabelBytes = 512U;
inline constexpr std::size_t maximumServiceActionLabelCodepoints = 128U;
inline constexpr std::size_t maximumServiceSenderBytes = 255U;
inline constexpr std::size_t maximumNotificationStateBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t maximumNotificationHistoryBytes = 8U * 1024U * 1024U;
inline constexpr auto maximumServiceTimeout = std::chrono::hours{24};

enum class ServiceUrgency : std::uint8_t { low, normal, critical };
enum class HistoryMode : std::uint8_t { disabled, session, persistent };
enum class ServiceTimeoutKind : std::uint8_t { configured_default, never, explicit_timeout };

struct ServiceTimeout final {
    ServiceTimeoutKind kind{ServiceTimeoutKind::configured_default};
    std::chrono::milliseconds duration{0};

    friend bool operator==(const ServiceTimeout &, const ServiceTimeout &) = default;
};

struct ServiceAction final {
    std::string id;
    std::string label;
    bool enabled{true};

    friend bool operator==(const ServiceAction &, const ServiceAction &) = default;
};

/// Fully decoded, still-untrusted input from the standard D-Bus boundary.
///
/// Image bytes and paths are deliberately absent. The boundary may validate and
/// discard such hints, but the authority never owns or persists them.
struct ServiceNotificationInput final {
    std::uint32_t replacesId{0U};
    std::optional<std::string> applicationName;
    std::optional<std::string> desktopFileId;
    std::string summary;
    std::string body;
    std::vector<ServiceAction> actions;
    ServiceUrgency urgency{ServiceUrgency::normal};
    ServiceTimeout timeout;
    bool transientHint{false};
    bool dismissible{true};
    std::string sender;
    std::uint64_t receivedAtUnixMilliseconds{0U};
};

/// Validates all decoded notification fields without reflecting their content
/// in diagnostics. It is a fuzzable boundary shared by the service and tests.
[[nodiscard]] foundation::Result<void>
validateServiceNotificationInput(const ServiceNotificationInput &input);

[[nodiscard]] bool isValidServiceActionId(std::string_view value) noexcept;
[[nodiscard]] bool isValidDesktopFileId(std::string_view value) noexcept;

} // namespace prismdrake::notifications
