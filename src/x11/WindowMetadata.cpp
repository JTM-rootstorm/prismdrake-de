#include "WindowMetadata.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> malformedMetadata() {
    return Result<Value>::failure(
        {ErrorCode::validation_error, "X11 window metadata is malformed.",
         "Discard the complete observation and retain the previous validated task record."});
}

template <typename Value> [[nodiscard]] Result<Value> oversizedMetadata() {
    return Result<Value>::failure(
        {ErrorCode::too_large, "X11 window metadata exceeds its fixed bounds.",
         "Discard the complete observation and retain the previous validated task record."});
}

[[nodiscard]] bool forbiddenTextCodePoint(std::uint32_t codePoint) noexcept {
    return codePoint == 0U || codePoint < 0x20U || (codePoint >= 0x7fU && codePoint <= 0x9fU);
}

struct Utf8Validation final {
    bool valid;
    std::size_t codePoints;
};

[[nodiscard]] Utf8Validation validateUtf8(std::string_view text) noexcept {
    std::size_t offset = 0U;
    std::size_t count = 0U;
    while (offset < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        std::uint32_t codePoint = 0U;
        std::size_t length = 0U;
        if (first <= 0x7fU) {
            codePoint = first;
            length = 1U;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            codePoint = first & 0x1fU;
            length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            codePoint = first & 0x0fU;
            length = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            codePoint = first & 0x07U;
            length = 4U;
        } else {
            return {false, 0U};
        }
        if (length > text.size() - offset) {
            return {false, 0U};
        }
        for (std::size_t index = 1U; index < length; ++index) {
            const auto next = static_cast<std::uint8_t>(text[offset + index]);
            if ((next & 0xc0U) != 0x80U) {
                return {false, 0U};
            }
            codePoint = (codePoint << 6U) | (next & 0x3fU);
        }
        if ((length == 3U && first == 0xe0U && codePoint < 0x800U) ||
            (length == 3U && first == 0xedU && codePoint >= 0xd800U) ||
            (length == 4U && first == 0xf0U && codePoint < 0x10000U) ||
            (length == 4U && first == 0xf4U && codePoint > 0x10ffffU) ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU) || forbiddenTextCodePoint(codePoint)) {
            return {false, 0U};
        }
        ++count;
        offset += length;
    }
    return {true, count};
}

[[nodiscard]] Result<std::string> checkedUtf8Title(std::string_view text) {
    if (text.size() > maximumWindowTitleBytes) {
        return oversizedMetadata<std::string>();
    }
    const auto validation = validateUtf8(text);
    if (!validation.valid) {
        return malformedMetadata<std::string>();
    }
    if (validation.codePoints > maximumWindowTitleCodePoints) {
        return oversizedMetadata<std::string>();
    }
    return Result<std::string>::success(std::string{text});
}

[[nodiscard]] Result<std::string> latin1ToUtf8(std::span<const std::uint8_t> bytes,
                                               std::size_t maximumBytes) {
    if (bytes.size() > maximumBytes) {
        return oversizedMetadata<std::string>();
    }
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        if (forbiddenTextCodePoint(byte)) {
            return malformedMetadata<std::string>();
        }
        if (byte < 0x80U) {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back(static_cast<char>(0xc0U | (byte >> 6U)));
            result.push_back(static_cast<char>(0x80U | (byte & 0x3fU)));
        }
    }
    return Result<std::string>::success(std::move(result));
}

[[nodiscard]] Result<std::string> decodeTitle(const WindowMetadataObservation &observation) {
    if (observation.utf8Title && !observation.utf8Title->empty()) {
        return checkedUtf8Title(observation.utf8Title.value());
    }
    if (observation.legacyTitle && !observation.legacyTitle->empty()) {
        const auto *data = reinterpret_cast<const std::uint8_t *>(observation.legacyTitle->data());
        auto title = latin1ToUtf8({data, observation.legacyTitle->size()}, maximumWindowTitleBytes);
        if (!title) {
            return title;
        }
        const auto validation = validateUtf8(title.value());
        if (!validation.valid) {
            return malformedMetadata<std::string>();
        }
        if (title.value().size() > maximumWindowTitleBytes ||
            validation.codePoints > maximumWindowTitleCodePoints) {
            return oversizedMetadata<std::string>();
        }
        return title;
    }
    return Result<std::string>::success("Untitled Window");
}

[[nodiscard]] Result<ApplicationIdentityEvidence>
decodeWmClass(const std::optional<std::vector<std::uint8_t>> &property) {
    if (!property) {
        return Result<ApplicationIdentityEvidence>::success(
            {ApplicationIdentitySource::genericUnknown, std::nullopt, std::nullopt,
             "unknown-application"});
    }
    const auto &bytes = property.value();
    if (bytes.size() > maximumWmClassBytes) {
        return oversizedMetadata<ApplicationIdentityEvidence>();
    }
    const auto firstEnd = std::find(bytes.begin(), bytes.end(), 0U);
    if (firstEnd == bytes.end()) {
        return malformedMetadata<ApplicationIdentityEvidence>();
    }
    const auto secondEnd = std::find(firstEnd + 1, bytes.end(), 0U);
    if (secondEnd == bytes.end() || secondEnd + 1 != bytes.end()) {
        return malformedMetadata<ApplicationIdentityEvidence>();
    }
    const auto instanceBytes = std::span<const std::uint8_t>{
        bytes.data(), static_cast<std::size_t>(firstEnd - bytes.begin())};
    const auto classOffset = static_cast<std::size_t>(firstEnd - bytes.begin()) + 1U;
    const auto classBytes = std::span<const std::uint8_t>{
        bytes.data() + classOffset, static_cast<std::size_t>(secondEnd - firstEnd - 1)};
    if (instanceBytes.size() > maximumWmClassPartBytes ||
        classBytes.size() > maximumWmClassPartBytes) {
        return oversizedMetadata<ApplicationIdentityEvidence>();
    }
    if (instanceBytes.empty() && classBytes.empty()) {
        return malformedMetadata<ApplicationIdentityEvidence>();
    }
    auto instance = latin1ToUtf8(instanceBytes, maximumWmClassPartBytes);
    auto className = latin1ToUtf8(classBytes, maximumWmClassPartBytes);
    if (!instance || !className) {
        return malformedMetadata<ApplicationIdentityEvidence>();
    }
    std::optional<std::string> optionalInstance =
        instance.value().empty() ? std::nullopt
                                 : std::optional<std::string>{std::move(instance).value()};
    std::optional<std::string> optionalClass =
        className.value().empty() ? std::nullopt
                                  : std::optional<std::string>{std::move(className).value()};
    const std::string groupingKey =
        optionalClass ? optionalClass.value() : optionalInstance.value();
    return Result<ApplicationIdentityEvidence>::success({ApplicationIdentitySource::wmClass,
                                                         std::move(optionalInstance),
                                                         std::move(optionalClass), groupingKey});
}

template <typename Value> [[nodiscard]] bool enumValueValid(Value value) noexcept {
    if constexpr (std::is_same_v<Value, WindowType>) {
        return value >= WindowType::normal && value <= WindowType::desktop;
    } else {
        return value >= WindowState::hidden && value <= WindowState::demandsAttention;
    }
}

template <typename Value>
[[nodiscard]] Result<std::vector<Value>> checkedEnums(const std::vector<Value> &values,
                                                      std::size_t maximum) {
    if (values.size() > maximum) {
        return oversizedMetadata<std::vector<Value>>();
    }
    std::vector<Value> checked;
    checked.reserve(values.size());
    for (const auto value : values) {
        if (!enumValueValid(value) ||
            std::find(checked.begin(), checked.end(), value) != checked.end()) {
            return malformedMetadata<std::vector<Value>>();
        }
        checked.push_back(value);
    }
    return Result<std::vector<Value>>::success(std::move(checked));
}

[[nodiscard]] Result<std::vector<WindowIcon>>
decodeIcons(const std::optional<std::vector<std::uint32_t>> &property) {
    if (!property || property->empty()) {
        return Result<std::vector<WindowIcon>>::success({});
    }
    const auto &words = property.value();
    if (words.size() > maximumNetWmIconBytes / sizeof(std::uint32_t)) {
        return oversizedMetadata<std::vector<WindowIcon>>();
    }

    std::vector<WindowIcon> icons;
    std::size_t offset = 0U;
    std::size_t aggregatePixels = 0U;
    while (offset < words.size()) {
        if (icons.size() >= maximumWindowIcons || words.size() - offset < 2U) {
            return icons.size() >= maximumWindowIcons
                       ? oversizedMetadata<std::vector<WindowIcon>>()
                       : malformedMetadata<std::vector<WindowIcon>>();
        }
        const auto width = words[offset];
        const auto height = words[offset + 1U];
        if (width == 0U || height == 0U) {
            return malformedMetadata<std::vector<WindowIcon>>();
        }
        if (width > maximumWindowIconDimension || height > maximumWindowIconDimension ||
            width > maximumWindowIconPixels / height) {
            return oversizedMetadata<std::vector<WindowIcon>>();
        }
        const auto pixels = static_cast<std::size_t>(width) * height;
        if (pixels > words.size() - offset - 2U) {
            return malformedMetadata<std::vector<WindowIcon>>();
        }
        if (aggregatePixels > maximumWindowIconPixels - pixels) {
            return oversizedMetadata<std::vector<WindowIcon>>();
        }
        aggregatePixels += pixels;
        const auto pixelBegin = words.begin() + static_cast<std::ptrdiff_t>(offset + 2U);
        icons.push_back({width, height,
                         std::vector<std::uint32_t>{
                             pixelBegin, pixelBegin + static_cast<std::ptrdiff_t>(pixels)}});
        offset += 2U + pixels;
    }
    return Result<std::vector<WindowIcon>>::success(std::move(icons));
}

} // namespace

Result<WindowMetadata> decodeWindowMetadata(const WindowMetadataObservation &observation) {
    auto title = decodeTitle(observation);
    auto identity = decodeWmClass(observation.wmClass);
    auto types = checkedEnums(observation.windowTypes, maximumWindowTypes);
    auto states = checkedEnums(observation.windowStates, maximumWindowStates);
    auto icons = decodeIcons(observation.netWmIcon);
    if (!title || !identity || !types || !states || !icons) {
        if (!title) {
            return Result<WindowMetadata>::failure(title.error());
        }
        if (!identity) {
            return Result<WindowMetadata>::failure(identity.error());
        }
        if (!types) {
            return Result<WindowMetadata>::failure(types.error());
        }
        if (!states) {
            return Result<WindowMetadata>::failure(states.error());
        }
        return Result<WindowMetadata>::failure(icons.error());
    }
    if (observation.transientFor && observation.transientFor.value() == observation.window) {
        return malformedMetadata<WindowMetadata>();
    }

    const auto hasState = [&states](WindowState state) {
        return std::find(states.value().begin(), states.value().end(), state) !=
               states.value().end();
    };
    const bool allWorkspaceAssignment =
        observation.workspace && observation.workspace.value() == allWorkspaces;
    const auto workspace = allWorkspaceAssignment ? std::nullopt : observation.workspace;
    const bool wmHintsUrgent =
        observation.wmHintsFlags && (observation.wmHintsFlags.value() & wmHintsUrgencyFlag) != 0U;
    const bool typeWasExplicit = !types.value().empty();
    const auto type = typeWasExplicit ? types.value().front() : WindowType::normal;
    const bool minimized = hasState(WindowState::hidden);
    const bool urgent = wmHintsUrgent || hasState(WindowState::demandsAttention);
    const bool skipTaskbar = hasState(WindowState::skipTaskbar);
    const bool modal = hasState(WindowState::modal);

    return Result<WindowMetadata>::success(WindowMetadata{
        observation.window,
        std::move(title).value(),
        std::move(identity).value(),
        type,
        typeWasExplicit,
        std::move(states).value(),
        workspace,
        allWorkspaceAssignment,
        minimized,
        urgent,
        skipTaskbar,
        modal,
        observation.transientFor,
        std::move(icons).value(),
    });
}

} // namespace prismdrake::x11
