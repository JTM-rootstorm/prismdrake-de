#include "NotificationModel.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace prismdrake::notifications {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using TimePoint = foundation::MonotonicClock::TimePoint;

enum class TextStatus : std::uint8_t {
    valid,
    invalidUtf8,
    unsafeControl,
    tooLarge,
};

[[nodiscard]] Error modelError(ErrorCode code, std::string message, std::string recovery) {
    return {code, std::move(message), std::move(recovery)};
}

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure(modelError(code, std::move(message), std::move(recovery)));
}

[[nodiscard]] TextStatus validateText(std::string_view text, std::size_t maximumBytes,
                                      std::size_t maximumCodepoints,
                                      bool allowBodyWhitespace) noexcept {
    if (text.size() > maximumBytes) {
        return TextStatus::tooLarge;
    }

    std::size_t codepoints = 0U;
    for (std::size_t offset = 0U; offset < text.size();) {
        const auto lead = static_cast<std::uint8_t>(text[offset]);
        std::size_t width = 0U;
        std::uint32_t codepoint = 0U;
        if (lead <= 0x7FU) {
            width = 1U;
            codepoint = lead;
        } else if ((lead & 0xE0U) == 0xC0U) {
            width = 2U;
            codepoint = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            width = 3U;
            codepoint = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            width = 4U;
            codepoint = lead & 0x07U;
        } else {
            return TextStatus::invalidUtf8;
        }
        if (width > text.size() - offset) {
            return TextStatus::invalidUtf8;
        }
        for (std::size_t index = 1U; index < width; ++index) {
            const auto byte = static_cast<std::uint8_t>(text[offset + index]);
            if ((byte & 0xC0U) != 0x80U) {
                return TextStatus::invalidUtf8;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((width == 2U && codepoint < 0x80U) || (width == 3U && codepoint < 0x800U) ||
            (width == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return TextStatus::invalidUtf8;
        }
        const bool allowedWhitespace =
            allowBodyWhitespace && (codepoint == '\t' || codepoint == '\n');
        if (codepoint == 0U || (codepoint < 0x20U && !allowedWhitespace) ||
            (codepoint >= 0x7FU && codepoint <= 0x9FU)) {
            return TextStatus::unsafeControl;
        }
        ++codepoints;
        if (codepoints > maximumCodepoints) {
            return TextStatus::tooLarge;
        }
        offset += width;
    }
    return TextStatus::valid;
}

[[nodiscard]] Result<void> checkedText(std::string_view text, std::size_t maximumBytes,
                                       std::size_t maximumCodepoints, bool allowBodyWhitespace,
                                       bool requireNonempty, std::string_view fieldName) {
    if (requireNonempty && text.empty()) {
        return Result<void>::failure(
            modelError(ErrorCode::validation_error,
                       "The synthetic notification " + std::string{fieldName} + " is empty.",
                       "Provide bounded nonempty literal plain text for the required field."));
    }
    switch (validateText(text, maximumBytes, maximumCodepoints, allowBodyWhitespace)) {
    case TextStatus::valid:
        return Result<void>::success();
    case TextStatus::tooLarge:
        return Result<void>::failure(modelError(
            ErrorCode::too_large,
            "The synthetic notification " + std::string{fieldName} + " exceeds its bounds.",
            "Provide smaller literal plain text within the documented byte and codepoint limits."));
    case TextStatus::invalidUtf8:
    case TextStatus::unsafeControl:
        return Result<void>::failure(modelError(
            ErrorCode::validation_error,
            "The synthetic notification " + std::string{fieldName} + " is invalid.",
            "Provide valid UTF-8 literal plain text without unsafe control characters."));
    }
    return Result<void>::failure(modelError(ErrorCode::validation_error,
                                            "The synthetic notification text is invalid.",
                                            "Discard the complete synthetic input."));
}

[[nodiscard]] bool isIdentifierCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
}

[[nodiscard]] bool validIdentifier(std::string_view value, std::size_t maximumBytes,
                                   bool allowColon) noexcept {
    if (value.empty() || value.size() > maximumBytes) {
        return false;
    }
    return std::ranges::all_of(value, [allowColon](char character) {
        return isIdentifierCharacter(character) || (allowColon && character == ':');
    });
}

[[nodiscard]] bool validUrgency(NotificationUrgency urgency) noexcept {
    switch (urgency) {
    case NotificationUrgency::low:
    case NotificationUrgency::normal:
    case NotificationUrgency::critical:
        return true;
    }
    return false;
}

[[nodiscard]] bool validPixelFormat(NotificationPixelFormat format) noexcept {
    switch (format) {
    case NotificationPixelFormat::rgb8:
    case NotificationPixelFormat::rgba8:
        return true;
    }
    return false;
}

[[nodiscard]] std::size_t imageBytes(const NotificationVisual &visual) noexcept {
    if (const auto *image = std::get_if<SyntheticNotificationImage>(&visual)) {
        return image->pixels.size();
    }
    return 0U;
}

[[nodiscard]] Result<void> validateVisual(const NotificationVisual &visual) {
    if (std::holds_alternative<std::monostate>(visual)) {
        return Result<void>::success();
    }
    if (const auto *icon = std::get_if<NotificationThemeIcon>(&visual)) {
        if (!validIdentifier(icon->name, maximumNotificationThemeIconNameBytes, false) ||
            icon->name == "." || icon->name == "..") {
            return Result<void>::failure(
                modelError(ErrorCode::validation_error,
                           "The synthetic notification theme icon identifier is invalid.",
                           "Use one bounded theme icon name without a path or URI scheme."));
        }
        return Result<void>::success();
    }

    const auto &image = std::get<SyntheticNotificationImage>(visual);
    if (!validPixelFormat(image.format) || image.width == 0U || image.height == 0U ||
        image.width > maximumNotificationImageDimension ||
        image.height > maximumNotificationImageDimension) {
        return Result<void>::failure(modelError(
            ErrorCode::validation_error, "The synthetic notification image metadata is invalid.",
            "Use one bounded nonempty RGB8 or RGBA8 synthetic image."));
    }
    const std::uint32_t channels = image.format == NotificationPixelFormat::rgb8 ? 3U : 4U;
    const auto expectedStride = image.width * channels;
    const auto expectedBytes = static_cast<std::size_t>(expectedStride) * image.height;
    if (image.rowStride != expectedStride || expectedBytes > maximumNotificationImageBytes ||
        image.pixels.size() != expectedBytes) {
        return Result<void>::failure(modelError(
            expectedBytes > maximumNotificationImageBytes ? ErrorCode::too_large
                                                          : ErrorCode::validation_error,
            "The synthetic notification image payload is invalid.",
            "Use tightly packed pixels matching the bounded dimensions and pixel format."));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void>
validateActions(const std::vector<SyntheticNotificationAction> &actions) {
    if (actions.size() > maximumNotificationActions) {
        return Result<void>::failure(modelError(ErrorCode::too_large,
                                                "The synthetic notification has too many actions.",
                                                "Use no more than the documented action limit."));
    }
    std::set<std::string_view> identifiers;
    for (const auto &action : actions) {
        if (!validIdentifier(action.id, maximumNotificationActionIdBytes, true) ||
            !identifiers.insert(action.id).second) {
            return Result<void>::failure(
                modelError(ErrorCode::validation_error,
                           "The synthetic notification action identifiers are invalid.",
                           "Use unique bounded ASCII action identifiers."));
        }
        auto label =
            checkedText(action.label, maximumNotificationActionLabelBytes,
                        maximumNotificationActionLabelCodepoints, false, true, "action label");
        if (!label) {
            return label;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::optional<TimePoint>> expiryFor(const NotificationTimeout &timeout,
                                                         const NotificationModelConfig &config,
                                                         TimePoint now) {
    std::chrono::milliseconds duration{0};
    switch (timeout.kind) {
    case NotificationTimeoutKind::defaultTimeout:
        if (timeout.duration != std::chrono::milliseconds::zero()) {
            return failure<std::optional<TimePoint>>(
                ErrorCode::validation_error,
                "The synthetic notification default timeout is ambiguous.",
                "Leave the duration at zero when selecting the configured default timeout.");
        }
        duration = config.defaultTimeout;
        break;
    case NotificationTimeoutKind::never:
        if (timeout.duration != std::chrono::milliseconds::zero()) {
            return failure<std::optional<TimePoint>>(
                ErrorCode::validation_error,
                "The synthetic notification non-expiring timeout is ambiguous.",
                "Leave the duration at zero when selecting a non-expiring notification.");
        }
        return Result<std::optional<TimePoint>>::success(std::nullopt);
    case NotificationTimeoutKind::explicitTimeout:
        if (timeout.duration <= std::chrono::milliseconds::zero() ||
            timeout.duration > maximumNotificationTimeout) {
            return failure<std::optional<TimePoint>>(
                ErrorCode::validation_error,
                "The synthetic notification explicit timeout is invalid.",
                "Use a positive duration no longer than the documented maximum.");
        }
        duration = timeout.duration;
        break;
    default:
        return failure<std::optional<TimePoint>>(
            ErrorCode::validation_error, "The synthetic notification timeout kind is invalid.",
            "Use a supported typed timeout policy.");
    }
    using WideDuration = std::chrono::duration<long double>;
    const auto requestedWide = std::chrono::duration_cast<WideDuration>(duration);
    const auto maximumWide =
        std::chrono::duration_cast<WideDuration>(foundation::MonotonicClock::Duration::max());
    if (requestedWide > maximumWide) {
        return failure<std::optional<TimePoint>>(
            ErrorCode::too_large, "The synthetic notification timeout cannot be represented.",
            "Use a smaller timeout or restart the synthetic clock from a reviewed epoch.");
    }
    const auto clockDuration =
        std::chrono::duration_cast<foundation::MonotonicClock::Duration>(duration);
    if (clockDuration <= foundation::MonotonicClock::Duration::zero() ||
        now > TimePoint::max() - clockDuration) {
        return failure<std::optional<TimePoint>>(
            ErrorCode::too_large, "The synthetic notification timeout cannot be represented.",
            "Use a smaller timeout or restart the synthetic clock from a reviewed epoch.");
    }
    return Result<std::optional<TimePoint>>::success(now + clockDuration);
}

[[nodiscard]] Result<void> validateInput(const SyntheticNotificationInput &input) {
    auto summary = checkedText(input.summary, maximumNotificationSummaryBytes,
                               maximumNotificationSummaryCodepoints, false, true, "summary");
    if (!summary) {
        return summary;
    }
    auto body = checkedText(input.body, maximumNotificationBodyBytes,
                            maximumNotificationBodyCodepoints, true, false, "body");
    if (!body) {
        return body;
    }
    if (input.applicationName) {
        auto applicationName = checkedText(
            *input.applicationName, maximumNotificationApplicationNameBytes,
            maximumNotificationApplicationNameCodepoints, false, true, "application name");
        if (!applicationName) {
            return applicationName;
        }
    }
    if (input.applicationId &&
        !validIdentifier(*input.applicationId, maximumNotificationApplicationIdBytes, false)) {
        return Result<void>::failure(modelError(
            ErrorCode::validation_error,
            "The synthetic notification application identifier is invalid.",
            "Use a bounded nonempty ASCII application identifier without a path or URI scheme."));
    }
    if (!validUrgency(input.urgency)) {
        return Result<void>::failure(modelError(ErrorCode::validation_error,
                                                "The synthetic notification urgency is invalid.",
                                                "Use low, normal, or critical urgency."));
    }
    auto visual = validateVisual(input.visual);
    if (!visual) {
        return visual;
    }
    return validateActions(input.actions);
}

[[nodiscard]] Result<void> validatePublishedCard(const NotificationCard &card,
                                                 foundation::Generation snapshotGeneration) {
    auto summary = checkedText(card.summary, maximumNotificationSummaryBytes,
                               maximumNotificationSummaryCodepoints, false, true, "summary");
    if (!summary) {
        return summary;
    }
    auto body = checkedText(card.body, maximumNotificationBodyBytes,
                            maximumNotificationBodyCodepoints, true, false, "body");
    if (!body) {
        return body;
    }
    if (card.applicationName) {
        auto applicationName = checkedText(
            *card.applicationName, maximumNotificationApplicationNameBytes,
            maximumNotificationApplicationNameCodepoints, false, true, "application name");
        if (!applicationName) {
            return applicationName;
        }
    }
    if (card.applicationId &&
        !validIdentifier(*card.applicationId, maximumNotificationApplicationIdBytes, false)) {
        return Result<void>::failure(modelError(
            ErrorCode::validation_error,
            "The synthetic notification application identifier is invalid.",
            "Use a bounded nonempty ASCII application identifier without a path or URI scheme."));
    }
    const auto maximumClockTimeout =
        std::chrono::duration_cast<foundation::MonotonicClock::Duration>(
            maximumNotificationTimeout);
    const bool invalidExpiry =
        card.expiresAt && (*card.expiresAt <= card.lastUpdatedAt ||
                           card.lastUpdatedAt > TimePoint::max() - maximumClockTimeout ||
                           *card.expiresAt > card.lastUpdatedAt + maximumClockTimeout);
    if (!validUrgency(card.urgency) || card.id.value() == 0U || !card.focusable ||
        card.accessibleRole != NotificationAccessibleRole::notification ||
        card.contentGeneration > snapshotGeneration || card.firstPresentedAt > card.lastUpdatedAt ||
        invalidExpiry) {
        return Result<void>::failure(modelError(
            ErrorCode::validation_error, "The synthetic notification published state is invalid.",
            "Retain the previous complete snapshot and publish model-owned card state."));
    }
    auto visual = validateVisual(card.visual);
    if (!visual) {
        return visual;
    }
    return validateActions(card.actions);
}

[[nodiscard]] Result<foundation::Generation> publishedGeneration(std::uint64_t value) {
    if (value == 0U) {
        return failure<foundation::Generation>(
            ErrorCode::too_large, "The synthetic notification generation space is exhausted.",
            "Restart the synthetic presentation harness through a reviewed recovery path.");
    }
    return foundation::Generation::fromPublished(value);
}

[[nodiscard]] std::uint64_t incrementIdentifier(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? 0U : value + 1U;
}

} // namespace

Result<void> validateNotificationSnapshot(const NotificationSnapshot &snapshot) {
    if (snapshot.cards.size() > maximumNotificationCards) {
        return Result<void>::failure(modelError(ErrorCode::too_large,
                                                "The synthetic notification has too many cards.",
                                                "Use no more than the documented card limit."));
    }

    std::set<NotificationId::Value> identifiers;
    std::size_t aggregateImageBytes = 0U;
    for (const auto &card : snapshot.cards) {
        if (!identifiers.insert(card.id.value()).second) {
            return Result<void>::failure(modelError(
                ErrorCode::validation_error,
                "The synthetic notification snapshot identifiers are invalid.",
                "Publish each nonzero model-owned notification identifier exactly once."));
        }
        auto valid = validatePublishedCard(card, snapshot.generation);
        if (!valid) {
            return valid;
        }
        const auto bytes = imageBytes(card.visual);
        if (bytes > maximumNotificationSnapshotImageBytes - aggregateImageBytes) {
            return Result<void>::failure(
                modelError(ErrorCode::too_large,
                           "The synthetic notification snapshot image payload exceeds its bound.",
                           "Use fewer or smaller synthetic images."));
        }
        aggregateImageBytes += bytes;
    }
    return Result<void>::success();
}

struct SyntheticNotificationModel::Impl final {
    std::shared_ptr<const foundation::MonotonicClock> clock;
    NotificationModelConfig config;
    std::uint64_t nextId{1U};
    std::uint64_t nextGeneration{1U};
    std::shared_ptr<const NotificationSnapshot> current;
};

SyntheticNotificationModel::SyntheticNotificationModel(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

SyntheticNotificationModel::~SyntheticNotificationModel() = default;
SyntheticNotificationModel::SyntheticNotificationModel(SyntheticNotificationModel &&) noexcept =
    default;
SyntheticNotificationModel &
SyntheticNotificationModel::operator=(SyntheticNotificationModel &&) noexcept = default;

Result<SyntheticNotificationModel>
SyntheticNotificationModel::create(std::shared_ptr<const foundation::MonotonicClock> clock,
                                   NotificationModelConfig config) {
    if (!clock || config.defaultTimeout <= std::chrono::milliseconds::zero() ||
        config.defaultTimeout > maximumNotificationTimeout) {
        return failure<SyntheticNotificationModel>(
            ErrorCode::invalid_argument,
            "The synthetic notification model configuration is invalid.",
            "Provide a clock and a positive bounded default timeout.");
    }
    return Result<SyntheticNotificationModel>::success(SyntheticNotificationModel{
        std::make_unique<Impl>(Impl{std::move(clock), config, 1U, 1U, nullptr})});
}

Result<NotificationUpsertOutcome>
SyntheticNotificationModel::upsert(SyntheticNotificationInput input) {
    if (!implementation_) {
        return failure<NotificationUpsertOutcome>(
            ErrorCode::invalid_argument, "The synthetic notification model was moved from.",
            "Use the model instance that owns the publication state.");
    }
    auto validated = validateInput(input);
    if (!validated) {
        return Result<NotificationUpsertOutcome>::failure(validated.error());
    }
    auto generation = publishedGeneration(implementation_->nextGeneration);
    if (!generation) {
        return Result<NotificationUpsertOutcome>::failure(generation.error());
    }
    const auto now = implementation_->clock->now();
    auto expiry = expiryFor(input.timeout, implementation_->config, now);
    if (!expiry) {
        return Result<NotificationUpsertOutcome>::failure(expiry.error());
    }

    std::vector<NotificationCard> cards = implementation_->current
                                              ? implementation_->current->cards
                                              : std::vector<NotificationCard>{};
    bool replaced = false;
    NotificationId id{0U};
    if (input.replacementId) {
        const auto existing = std::ranges::find_if(cards, [&input](const NotificationCard &card) {
            return card.id == *input.replacementId;
        });
        if (existing == cards.end()) {
            return failure<NotificationUpsertOutcome>(
                ErrorCode::not_found,
                "The synthetic notification replacement target does not exist.",
                "Discard the stale identifier or publish a new synthetic notification.");
        }
        id = existing->id;
        const auto firstPresentedAt = existing->firstPresentedAt;
        *existing = NotificationCard{id,
                                     std::move(input.summary),
                                     std::move(input.body),
                                     std::move(input.applicationName),
                                     std::move(input.applicationId),
                                     input.urgency,
                                     std::move(input.visual),
                                     std::move(input.actions),
                                     true,
                                     input.dismissible,
                                     NotificationAccessibleRole::notification,
                                     generation.value(),
                                     firstPresentedAt,
                                     now,
                                     expiry.value()};
        replaced = true;
    } else {
        if (cards.size() >= maximumNotificationCards) {
            return failure<NotificationUpsertOutcome>(
                ErrorCode::too_large, "The synthetic notification card limit was reached.",
                "Dismiss or expire a card before publishing another synthetic notification.");
        }
        if (implementation_->nextId == 0U) {
            return failure<NotificationUpsertOutcome>(
                ErrorCode::too_large, "The synthetic notification identifier space is exhausted.",
                "Restart the synthetic presentation harness through a reviewed recovery path.");
        }
        id = NotificationId{implementation_->nextId};
        cards.push_back(
            NotificationCard{id, std::move(input.summary), std::move(input.body),
                             std::move(input.applicationName), std::move(input.applicationId),
                             input.urgency, std::move(input.visual), std::move(input.actions), true,
                             input.dismissible, NotificationAccessibleRole::notification,
                             generation.value(), now, now, expiry.value()});
    }

    std::size_t aggregateImageBytes = 0U;
    for (const auto &card : cards) {
        const auto bytes = imageBytes(card.visual);
        if (bytes > maximumNotificationSnapshotImageBytes - aggregateImageBytes) {
            return failure<NotificationUpsertOutcome>(
                ErrorCode::too_large,
                "The synthetic notification snapshot image payload exceeds its bound.",
                "Use fewer or smaller synthetic images.");
        }
        aggregateImageBytes += bytes;
    }

    auto snapshot = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{generation.value(), std::move(cards)});
    if (!replaced) {
        implementation_->nextId = incrementIdentifier(implementation_->nextId);
    }
    implementation_->nextGeneration = incrementIdentifier(implementation_->nextGeneration);
    implementation_->current = snapshot;
    return Result<NotificationUpsertOutcome>::success(
        NotificationUpsertOutcome{std::move(snapshot), id, replaced});
}

Result<NotificationPublicationOutcome>
SyntheticNotificationModel::dismiss(NotificationId id,
                                    foundation::Generation expectedContentGeneration) {
    if (!implementation_) {
        return failure<NotificationPublicationOutcome>(
            ErrorCode::invalid_argument, "The synthetic notification model was moved from.",
            "Use the model instance that owns the publication state.");
    }
    if (!implementation_->current) {
        return failure<NotificationPublicationOutcome>(
            ErrorCode::not_found, "The synthetic notification card does not exist.",
            "Discard the stale identifier.");
    }
    auto cards = implementation_->current->cards;
    const auto selected =
        std::ranges::find_if(cards, [id](const NotificationCard &card) { return card.id == id; });
    if (selected == cards.end()) {
        return failure<NotificationPublicationOutcome>(
            ErrorCode::not_found, "The synthetic notification card does not exist.",
            "Discard the stale identifier.");
    }
    if (selected->contentGeneration != expectedContentGeneration) {
        return failure<NotificationPublicationOutcome>(
            ErrorCode::cancelled, "The synthetic notification dismiss request is stale.",
            "Discard the stale request and dismiss the current card content.");
    }
    auto generation = publishedGeneration(implementation_->nextGeneration);
    if (!generation) {
        return Result<NotificationPublicationOutcome>::failure(generation.error());
    }
    cards.erase(selected);
    auto snapshot = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{generation.value(), std::move(cards)});
    implementation_->nextGeneration = incrementIdentifier(implementation_->nextGeneration);
    implementation_->current = snapshot;
    return Result<NotificationPublicationOutcome>::success(
        NotificationPublicationOutcome{std::move(snapshot), true, {id}});
}

Result<NotificationPublicationOutcome> SyntheticNotificationModel::advanceTimeouts() {
    if (!implementation_) {
        return failure<NotificationPublicationOutcome>(
            ErrorCode::invalid_argument, "The synthetic notification model was moved from.",
            "Use the model instance that owns the publication state.");
    }
    if (!implementation_->current) {
        return Result<NotificationPublicationOutcome>::success(
            NotificationPublicationOutcome{nullptr, false, {}});
    }
    const auto now = implementation_->clock->now();
    std::vector<NotificationId> removed;
    std::vector<NotificationCard> retained;
    retained.reserve(implementation_->current->cards.size());
    for (const auto &card : implementation_->current->cards) {
        if (card.expiresAt && *card.expiresAt <= now) {
            removed.push_back(card.id);
        } else {
            retained.push_back(card);
        }
    }
    if (removed.empty()) {
        return Result<NotificationPublicationOutcome>::success(
            NotificationPublicationOutcome{implementation_->current, false, {}});
    }
    auto generation = publishedGeneration(implementation_->nextGeneration);
    if (!generation) {
        return Result<NotificationPublicationOutcome>::failure(generation.error());
    }
    auto snapshot = std::make_shared<const NotificationSnapshot>(
        NotificationSnapshot{generation.value(), std::move(retained)});
    implementation_->nextGeneration = incrementIdentifier(implementation_->nextGeneration);
    implementation_->current = snapshot;
    return Result<NotificationPublicationOutcome>::success(
        NotificationPublicationOutcome{std::move(snapshot), true, std::move(removed)});
}

Result<NotificationActionActivation>
SyntheticNotificationModel::activateAction(NotificationId id,
                                           foundation::Generation expectedContentGeneration,
                                           std::string_view actionId) const {
    if (!implementation_ || !implementation_->current) {
        return failure<NotificationActionActivation>(
            ErrorCode::not_found, "The synthetic notification action does not exist.",
            "Discard the stale notification or action identifier.");
    }
    const auto card = std::ranges::find_if(
        implementation_->current->cards,
        [id](const NotificationCard &candidate) { return candidate.id == id; });
    if (card == implementation_->current->cards.end()) {
        return failure<NotificationActionActivation>(
            ErrorCode::not_found, "The synthetic notification action does not exist.",
            "Discard the stale notification or action identifier.");
    }
    if (card->contentGeneration != expectedContentGeneration) {
        return failure<NotificationActionActivation>(
            ErrorCode::cancelled, "The synthetic notification action request is stale.",
            "Discard the stale request and activate an action from the current card content.");
    }
    const auto action = std::ranges::find_if(
        card->actions, [actionId](const auto &candidate) { return candidate.id == actionId; });
    if (action == card->actions.end()) {
        return failure<NotificationActionActivation>(
            ErrorCode::not_found, "The synthetic notification action does not exist.",
            "Discard the stale notification or action identifier.");
    }
    if (!action->enabled) {
        return failure<NotificationActionActivation>(
            ErrorCode::permission_denied, "The synthetic notification action is disabled.",
            "Choose an enabled action.");
    }
    return Result<NotificationActionActivation>::success(
        NotificationActionActivation{id, card->contentGeneration, action->id});
}

Result<std::vector<NotificationFocusTarget>>
SyntheticNotificationModel::focusTargets(NotificationId id) const {
    if (!implementation_ || !implementation_->current) {
        return failure<std::vector<NotificationFocusTarget>>(
            ErrorCode::not_found, "The synthetic notification card does not exist.",
            "Discard the stale notification identifier.");
    }
    const auto card = std::ranges::find_if(
        implementation_->current->cards,
        [id](const NotificationCard &candidate) { return candidate.id == id; });
    if (card == implementation_->current->cards.end()) {
        return failure<std::vector<NotificationFocusTarget>>(
            ErrorCode::not_found, "The synthetic notification card does not exist.",
            "Discard the stale notification identifier.");
    }
    std::vector<NotificationFocusTarget> targets;
    targets.reserve(card->actions.size() + 2U);
    targets.push_back({NotificationFocusTargetKind::card, NotificationAccessibleRole::notification,
                       std::nullopt});
    for (std::size_t index = 0U; index < card->actions.size(); ++index) {
        if (card->actions[index].enabled) {
            targets.push_back(
                {NotificationFocusTargetKind::action, NotificationAccessibleRole::button, index});
        }
    }
    if (card->dismissible) {
        targets.push_back({NotificationFocusTargetKind::dismiss, NotificationAccessibleRole::button,
                           std::nullopt});
    }
    return Result<std::vector<NotificationFocusTarget>>::success(std::move(targets));
}

std::shared_ptr<const NotificationSnapshot> SyntheticNotificationModel::current() const noexcept {
    return implementation_ ? implementation_->current : nullptr;
}

} // namespace prismdrake::notifications
