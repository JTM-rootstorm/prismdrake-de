#pragma once

#include "MonotonicClock.hpp"
#include "NotificationTypes.hpp"
#include "Result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prismdrake::notifyd {

enum class CloseReason : std::uint32_t {
    expired = 1U,
    dismissed = 2U,
    requested = 3U,
    undefined = 4U,
};

struct NotificationRecord final {
    std::uint64_t recordId{0U};
    std::uint64_t contentGeneration{0U};
    std::optional<std::uint32_t> notificationId;
    std::optional<std::string> applicationName;
    std::optional<std::string> desktopFileId;
    std::string summary;
    std::string body;
    notifications::ServiceUrgency urgency{notifications::ServiceUrgency::normal};
    std::uint64_t receivedAtUnixMilliseconds{0U};
    bool dismissible{true};
    bool transientPresentation{false};
    bool transientHint{false};
    std::vector<notifications::ServiceAction> actions;
    std::optional<std::string> sender;
    std::optional<foundation::MonotonicClock::TimePoint> expiresAt;

    friend bool operator==(const NotificationRecord &, const NotificationRecord &) = default;
};

struct AuthorityOptions final {
    notifications::HistoryMode historyMode{notifications::HistoryMode::session};
    std::chrono::milliseconds defaultTimeout{std::chrono::seconds{5}};
    bool configuredDoNotDisturb{false};
    bool notificationsEnabled{true};
    std::optional<std::filesystem::path> persistentHistoryPath;
};

struct CloseEvent final {
    std::uint32_t notificationId{0U};
    CloseReason reason{CloseReason::undefined};
    std::optional<std::string> destination;
};

struct ActionEvent final {
    std::uint32_t notificationId{0U};
    std::string actionId;
    std::string destination;
};

struct NotifyOutcome final {
    std::uint32_t notificationId{0U};
    std::uint64_t recordId{0U};
    std::uint64_t contentGeneration{0U};
    std::uint64_t stateGeneration{0U};
    bool replaced{false};
};

struct CloseOutcome final {
    std::uint64_t stateGeneration{0U};
    std::optional<CloseEvent> event;
};

struct MutationOutcome final {
    std::uint64_t stateGeneration{0U};
    bool published{false};
    std::vector<CloseEvent> closeEvents;
};

struct SettingsInput final {
    notifications::HistoryMode historyMode{notifications::HistoryMode::session};
    std::chrono::milliseconds defaultTimeout{std::chrono::seconds{5}};
    bool configuredDoNotDisturb{false};
    bool notificationsEnabled{true};
};

/// Single-threaded toolkit-neutral notification authority.
///
/// The owner drives it from one D-Bus event loop. No method performs logging,
/// rendering, image decoding, command execution, or asynchronous work.
class NotificationAuthority final {
  public:
    NotificationAuthority(const NotificationAuthority &) = delete;
    NotificationAuthority &operator=(const NotificationAuthority &) = delete;
    NotificationAuthority(NotificationAuthority &&) noexcept = default;
    NotificationAuthority &operator=(NotificationAuthority &&) noexcept = default;

    [[nodiscard]] static foundation::Result<NotificationAuthority>
    create(std::shared_ptr<const foundation::MonotonicClock> clock, AuthorityOptions options);

    [[nodiscard]] foundation::Result<NotifyOutcome>
    notify(notifications::ServiceNotificationInput input);
    [[nodiscard]] foundation::Result<CloseOutcome> close(std::uint32_t notificationId,
                                                         CloseReason reason);
    [[nodiscard]] foundation::Result<MutationOutcome> advanceTimeouts();
    [[nodiscard]] foundation::Result<MutationOutcome> senderDisappeared(std::string_view sender);

    [[nodiscard]] foundation::Result<MutationOutcome> dismiss(std::uint64_t recordId,
                                                              std::uint64_t contentGeneration);
    [[nodiscard]] foundation::Result<ActionEvent> invokeAction(std::uint64_t recordId,
                                                               std::uint64_t contentGeneration,
                                                               std::string_view actionId) const;
    [[nodiscard]] foundation::Result<std::uint64_t> setDoNotDisturb(bool enabled);
    [[nodiscard]] foundation::Result<std::uint64_t> clearHistory();
    [[nodiscard]] foundation::Result<std::uint64_t> applySettings(SettingsInput settings);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] notifications::HistoryMode historyMode() const noexcept { return history_mode_; }
    [[nodiscard]] bool effectiveDoNotDisturb() const noexcept;
    [[nodiscard]] std::optional<bool> runtimeDoNotDisturbOverride() const noexcept {
        return runtime_do_not_disturb_override_;
    }
    [[nodiscard]] const std::vector<NotificationRecord> &records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::optional<foundation::MonotonicClock::TimePoint> nextExpiry() const noexcept;

    /// Flushes persistent closed history. Other modes are a successful no-op.
    [[nodiscard]] foundation::Result<void> flushHistory() const;

    /// Returns and clears one redacted persistence-degradation latch.
    [[nodiscard]] bool takePersistenceDegradation() noexcept;

  private:
    NotificationAuthority(std::shared_ptr<const foundation::MonotonicClock> clock,
                          AuthorityOptions options, std::vector<NotificationRecord> records,
                          std::uint64_t nextRecordId) noexcept;

    [[nodiscard]] foundation::Result<std::uint32_t> allocateNotificationId();
    [[nodiscard]] foundation::Result<std::uint64_t> allocateRecordId();
    [[nodiscard]] foundation::Result<void> ensurePublishable() const;
    [[nodiscard]] foundation::Result<void> publish();
    [[nodiscard]] foundation::Result<void> persistIfNeeded() const;
    [[nodiscard]] foundation::Result<void>
    persistCandidate(const std::vector<NotificationRecord> &records) const;
    void makeCapacityForOne();
    void refreshTransientPresentation(NotificationRecord &record) const noexcept;
    [[nodiscard]] std::vector<NotificationRecord>::iterator findRecord(std::uint64_t recordId);
    [[nodiscard]] std::vector<NotificationRecord>::const_iterator
    findRecord(std::uint64_t recordId) const;

    std::shared_ptr<const foundation::MonotonicClock> clock_;
    notifications::HistoryMode history_mode_;
    std::chrono::milliseconds default_timeout_;
    bool configured_do_not_disturb_;
    bool notifications_enabled_;
    std::optional<bool> runtime_do_not_disturb_override_;
    std::optional<std::filesystem::path> persistent_history_path_;
    std::vector<NotificationRecord> records_;
    std::uint32_t next_notification_id_{1U};
    std::uint64_t next_record_id_{1U};
    std::uint64_t generation_{1U};
    bool persistence_degraded_{false};
    bool settings_observed_{false};
};

} // namespace prismdrake::notifyd
