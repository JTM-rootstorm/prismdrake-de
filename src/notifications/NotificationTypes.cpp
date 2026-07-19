#include "NotificationTypes.hpp"

#include <algorithm>
#include <set>

namespace prismdrake::notifications {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class TextStatus : std::uint8_t { valid, invalid, too_large };

[[nodiscard]] Error validationError(ErrorCode code, std::string message) {
    return {code, std::move(message), "Discard the complete notification request."};
}

[[nodiscard]] TextStatus validateText(std::string_view text, std::size_t maximumBytes,
                                      std::size_t maximumCodepoints, bool allowBodyWhitespace,
                                      bool allowControlCharacters = false) noexcept {
    if (text.size() > maximumBytes) {
        return TextStatus::too_large;
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
            return TextStatus::invalid;
        }
        if (width > text.size() - offset) {
            return TextStatus::invalid;
        }
        for (std::size_t index = 1U; index < width; ++index) {
            const auto byte = static_cast<std::uint8_t>(text[offset + index]);
            if ((byte & 0xC0U) != 0x80U) {
                return TextStatus::invalid;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((width == 2U && codepoint < 0x80U) || (width == 3U && codepoint < 0x800U) ||
            (width == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return TextStatus::invalid;
        }
        const bool bodyWhitespace = allowBodyWhitespace && (codepoint == '\t' || codepoint == '\n');
        if (codepoint == 0U ||
            (!allowControlCharacters && ((codepoint < 0x20U && !bodyWhitespace) ||
                                         (codepoint >= 0x7FU && codepoint <= 0x9FU)))) {
            return TextStatus::invalid;
        }
        ++codepoints;
        if (codepoints > maximumCodepoints) {
            return TextStatus::too_large;
        }
        offset += width;
    }
    return TextStatus::valid;
}

[[nodiscard]] Result<void> checkedText(std::string_view value, std::size_t maximumBytes,
                                       std::size_t maximumCodepoints, bool allowBodyWhitespace,
                                       bool requireNonempty) {
    if (requireNonempty && value.empty()) {
        return Result<void>::failure(validationError(ErrorCode::validation_error,
                                                     "A required notification field is empty."));
    }
    switch (validateText(value, maximumBytes, maximumCodepoints, allowBodyWhitespace)) {
    case TextStatus::valid:
        return Result<void>::success();
    case TextStatus::invalid:
        return Result<void>::failure(validationError(
            ErrorCode::validation_error, "A notification text field is not valid plain UTF-8."));
    case TextStatus::too_large:
        return Result<void>::failure(validationError(
            ErrorCode::too_large, "A notification text field exceeds its documented bound."));
    }
    return Result<void>::failure(
        validationError(ErrorCode::validation_error, "A notification field is invalid."));
}

[[nodiscard]] bool validSender(std::string_view value) noexcept {
    if (value.size() < 3U || value.size() > maximumServiceSenderBytes || value.front() != ':') {
        return false;
    }
    return std::ranges::all_of(value.substr(1U), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}

} // namespace

bool isValidServiceActionId(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximumServiceActionIdBytes) {
        return false;
    }
    return validateText(value, maximumServiceActionIdBytes, maximumServiceActionIdCodepoints, false,
                        true) == TextStatus::valid;
}

bool isValidDesktopFileId(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximumServiceDesktopFileIdBytes ||
        !value.ends_with(".desktop") || value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        return false;
    }
    return validateText(value, maximumServiceDesktopFileIdBytes,
                        maximumServiceDesktopFileIdCodepoints, false) == TextStatus::valid;
}

Result<void> validateServiceNotificationInput(const ServiceNotificationInput &input) {
    if (!validSender(input.sender)) {
        return Result<void>::failure(validationError(
            ErrorCode::validation_error, "The notification sender identity is invalid."));
    }
    if (input.applicationName) {
        auto checked = checkedText(*input.applicationName, maximumServiceApplicationNameBytes,
                                   maximumServiceApplicationNameCodepoints, false, false);
        if (!checked) {
            return checked;
        }
    }
    if (input.desktopFileId && !isValidDesktopFileId(*input.desktopFileId)) {
        return Result<void>::failure(validationError(
            ErrorCode::validation_error, "The notification desktop-file identifier is invalid."));
    }
    auto summary = checkedText(input.summary, maximumServiceSummaryBytes,
                               maximumServiceSummaryCodepoints, false, false);
    if (!summary) {
        return summary;
    }
    auto body =
        checkedText(input.body, maximumServiceBodyBytes, maximumServiceBodyCodepoints, true, false);
    if (!body) {
        return body;
    }
    if (input.actions.size() > maximumServiceActions) {
        return Result<void>::failure(
            validationError(ErrorCode::too_large, "The notification has too many actions."));
    }
    std::set<std::string_view> actionIds;
    for (const auto &action : input.actions) {
        if (!isValidServiceActionId(action.id) || !actionIds.insert(action.id).second) {
            return Result<void>::failure(validationError(
                ErrorCode::validation_error, "The notification action identifiers are invalid."));
        }
        auto label = checkedText(action.label, maximumServiceActionLabelBytes,
                                 maximumServiceActionLabelCodepoints, false, true);
        if (!label) {
            return label;
        }
    }
    switch (input.urgency) {
    case ServiceUrgency::low:
    case ServiceUrgency::normal:
    case ServiceUrgency::critical:
        break;
    default:
        return Result<void>::failure(
            validationError(ErrorCode::validation_error, "The notification urgency is invalid."));
    }
    switch (input.timeout.kind) {
    case ServiceTimeoutKind::configured_default:
    case ServiceTimeoutKind::never:
        if (input.timeout.duration != std::chrono::milliseconds::zero()) {
            return Result<void>::failure(validationError(ErrorCode::validation_error,
                                                         "The notification timeout is ambiguous."));
        }
        break;
    case ServiceTimeoutKind::explicit_timeout:
        if (input.timeout.duration <= std::chrono::milliseconds::zero() ||
            input.timeout.duration > maximumServiceTimeout) {
            return Result<void>::failure(validationError(
                input.timeout.duration > maximumServiceTimeout ? ErrorCode::too_large
                                                               : ErrorCode::validation_error,
                "The notification timeout is outside its documented bound."));
        }
        break;
    default:
        return Result<void>::failure(validationError(ErrorCode::validation_error,
                                                     "The notification timeout kind is invalid."));
    }
    return Result<void>::success();
}

} // namespace prismdrake::notifications
