#pragma once

#include "Generation.hpp"
#include "MonotonicClock.hpp"
#include "Result.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::notifications {

inline constexpr std::size_t maximumNotificationCards = 32U;
inline constexpr std::size_t maximumNotificationSummaryBytes = 1024U;
inline constexpr std::size_t maximumNotificationSummaryCodepoints = 256U;
inline constexpr std::size_t maximumNotificationBodyBytes = 16U * 1024U;
inline constexpr std::size_t maximumNotificationBodyCodepoints = 4096U;
inline constexpr std::size_t maximumNotificationApplicationNameBytes = 512U;
inline constexpr std::size_t maximumNotificationApplicationNameCodepoints = 128U;
inline constexpr std::size_t maximumNotificationApplicationIdBytes = 255U;
inline constexpr std::size_t maximumNotificationActions = 8U;
inline constexpr std::size_t maximumNotificationActionIdBytes = 64U;
inline constexpr std::size_t maximumNotificationActionLabelBytes = 512U;
inline constexpr std::size_t maximumNotificationActionLabelCodepoints = 128U;
inline constexpr std::size_t maximumNotificationThemeIconNameBytes = 255U;
inline constexpr std::uint32_t maximumNotificationImageDimension = 512U;
inline constexpr std::size_t maximumNotificationImageBytes = 1024U * 1024U;
inline constexpr std::size_t maximumNotificationSnapshotImageBytes = 8U * 1024U * 1024U;
inline constexpr auto maximumNotificationTimeout = std::chrono::hours{24};

enum class NotificationUrgency : std::uint8_t {
    low,
    normal,
    critical,
};

/// Stable nonzero identity allocated only by the synthetic model owner.
class NotificationId final {
  public:
    using Value = std::uint64_t;

    [[nodiscard]] Value value() const noexcept { return value_; }

    friend auto operator<=>(const NotificationId &, const NotificationId &) = default;

  private:
    explicit NotificationId(Value value) noexcept : value_(value) {}

    Value value_;

    friend class SyntheticNotificationModel;
};

/// A safe icon-theme lookup key. Paths, URI schemes, and image loading remain outside this model.
struct NotificationThemeIcon final {
    std::string name;

    friend bool operator==(const NotificationThemeIcon &, const NotificationThemeIcon &) = default;
};

enum class NotificationPixelFormat : std::uint8_t {
    rgb8,
    rgba8,
};

/// Already-decoded, tightly packed synthetic pixels with no file or network origin.
struct SyntheticNotificationImage final {
    NotificationPixelFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t rowStride;
    std::vector<std::uint8_t> pixels;

    friend bool operator==(const SyntheticNotificationImage &,
                           const SyntheticNotificationImage &) = default;
};

using NotificationVisual =
    std::variant<std::monostate, NotificationThemeIcon, SyntheticNotificationImage>;

/// One bounded action. The label is its accessible button name.
struct SyntheticNotificationAction final {
    std::string id;
    std::string label;
    bool enabled{true};

    friend bool operator==(const SyntheticNotificationAction &,
                           const SyntheticNotificationAction &) = default;
};

enum class NotificationTimeoutKind : std::uint8_t {
    defaultTimeout,
    never,
    explicitTimeout,
};

struct NotificationTimeout final {
    NotificationTimeoutKind kind{NotificationTimeoutKind::defaultTimeout};
    std::chrono::milliseconds duration{0};

    friend bool operator==(const NotificationTimeout &, const NotificationTimeout &) = default;
};

/// Synthetic-only untrusted input. Text remains literal plain text; no markup is interpreted.
struct SyntheticNotificationInput final {
    std::optional<NotificationId> replacementId;
    std::string summary;
    std::string body;
    std::optional<std::string> applicationName;
    std::optional<std::string> applicationId;
    NotificationUrgency urgency{NotificationUrgency::normal};
    NotificationVisual visual;
    std::vector<SyntheticNotificationAction> actions;
    NotificationTimeout timeout;
    bool dismissible{true};

    friend bool operator==(const SyntheticNotificationInput &,
                           const SyntheticNotificationInput &) = default;
};

struct NotificationModelConfig final {
    std::chrono::milliseconds defaultTimeout{std::chrono::seconds{5}};

    friend bool operator==(const NotificationModelConfig &,
                           const NotificationModelConfig &) = default;
};

enum class NotificationAccessibleRole : std::uint8_t {
    notification,
    button,
};

/// Ordered keyboard focus targets. Action indices refer to NotificationCard::actions.
enum class NotificationFocusTargetKind : std::uint8_t {
    card,
    action,
    dismiss,
};

struct NotificationFocusTarget final {
    NotificationFocusTargetKind kind;
    NotificationAccessibleRole accessibleRole;
    std::optional<std::size_t> actionIndex;

    friend bool operator==(const NotificationFocusTarget &,
                           const NotificationFocusTarget &) = default;
};

/// Immutable card data published to a presentation adapter.
///
/// summary is the accessible notification name, body is its accessible description, urgency is a
/// typed non-color state, and each action label is its accessible button name.
struct NotificationCard final {
    NotificationId id;
    std::string summary;
    std::string body;
    std::optional<std::string> applicationName;
    std::optional<std::string> applicationId;
    NotificationUrgency urgency;
    NotificationVisual visual;
    std::vector<SyntheticNotificationAction> actions;
    bool focusable;
    bool dismissible;
    NotificationAccessibleRole accessibleRole;
    foundation::Generation contentGeneration;
    foundation::MonotonicClock::TimePoint firstPresentedAt;
    foundation::MonotonicClock::TimePoint lastUpdatedAt;
    std::optional<foundation::MonotonicClock::TimePoint> expiresAt;

    friend bool operator==(const NotificationCard &, const NotificationCard &) = default;
};

/// One complete immutable synthetic-card publication.
struct NotificationSnapshot final {
    foundation::Generation generation;
    std::vector<NotificationCard> cards;

    friend bool operator==(const NotificationSnapshot &, const NotificationSnapshot &) = default;
};

/// Revalidates a complete published snapshot before it crosses into another in-process layer.
///
/// SyntheticNotificationModel publications satisfy this contract by construction. The explicit
/// validator prevents copied or test-constructed snapshots from bypassing the same text, image,
/// identifier, generation, accessibility, and aggregate bounds at a presentation boundary.
[[nodiscard]] foundation::Result<void>
validateNotificationSnapshot(const NotificationSnapshot &snapshot);

struct NotificationUpsertOutcome final {
    std::shared_ptr<const NotificationSnapshot> snapshot;
    NotificationId id;
    bool replaced;

    friend bool operator==(const NotificationUpsertOutcome &,
                           const NotificationUpsertOutcome &) = default;
};

struct NotificationPublicationOutcome final {
    std::shared_ptr<const NotificationSnapshot> snapshot;
    bool published;
    std::vector<NotificationId> removedIds;

    friend bool operator==(const NotificationPublicationOutcome &,
                           const NotificationPublicationOutcome &) = default;
};

/// Typed action selection only; this model never invokes sender code or another process.
struct NotificationActionActivation final {
    NotificationId notificationId;
    foundation::Generation contentGeneration;
    std::string actionId;

    friend bool operator==(const NotificationActionActivation &,
                           const NotificationActionActivation &) = default;
};

/// Single-owner display-free PD1 synthetic notification-card model.
///
/// The model performs no I/O, D-Bus, logging, history, image decoding, rendering, or threading.
/// Captured snapshot pointers preserve immutable lifetime.
class SyntheticNotificationModel final {
  public:
    ~SyntheticNotificationModel();

    SyntheticNotificationModel(const SyntheticNotificationModel &) = delete;
    SyntheticNotificationModel &operator=(const SyntheticNotificationModel &) = delete;
    SyntheticNotificationModel(SyntheticNotificationModel &&) noexcept;
    SyntheticNotificationModel &operator=(SyntheticNotificationModel &&) noexcept;

    [[nodiscard]] static foundation::Result<SyntheticNotificationModel>
    create(std::shared_ptr<const foundation::MonotonicClock> clock,
           NotificationModelConfig config = {});

    [[nodiscard]] foundation::Result<NotificationUpsertOutcome>
    upsert(SyntheticNotificationInput input);

    [[nodiscard]] foundation::Result<NotificationPublicationOutcome>
    dismiss(NotificationId id, foundation::Generation expectedContentGeneration);

    /// Removes every card whose inclusive deadline is at or before the injected clock's time.
    [[nodiscard]] foundation::Result<NotificationPublicationOutcome> advanceTimeouts();

    [[nodiscard]] foundation::Result<NotificationActionActivation>
    activateAction(NotificationId id, foundation::Generation expectedContentGeneration,
                   std::string_view actionId) const;

    /// Returns card, enabled actions in input order, then dismiss, when available.
    [[nodiscard]] foundation::Result<std::vector<NotificationFocusTarget>>
    focusTargets(NotificationId id) const;

    [[nodiscard]] std::shared_ptr<const NotificationSnapshot> current() const noexcept;

  private:
    struct Impl;
    explicit SyntheticNotificationModel(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace prismdrake::notifications
