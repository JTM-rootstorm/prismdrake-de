#include "DesktopEntryParser.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

constexpr std::string_view desktopEntryGroup = "Desktop Entry";

enum class Utf8Status : std::uint8_t { valid, invalid, tooManyCodepoints };

struct KeyParts final {
    std::string_view base;
    std::optional<std::string> locale;
};

struct ParsedFields final {
    std::optional<std::string> type;
    std::map<std::string, std::string> names;
    std::map<std::string, std::string> genericNames;
    std::map<std::string, std::string> comments;
    std::map<std::string, std::string> icons;
    std::map<std::string, std::vector<std::string>> keywords;
    std::vector<std::string> categories;
    std::optional<std::string> exec;
    std::optional<std::string> tryExec;
    std::optional<std::string> path;
    bool terminal{false};
    bool hidden{false};
    bool noDisplay{false};
    bool dbusActivatable{false};
    std::optional<std::vector<std::string>> onlyShowIn;
    std::optional<std::vector<std::string>> notShowIn;
};

[[nodiscard]] Result<DesktopEntry> failure(ErrorCode code, std::string message,
                                           std::string recovery) {
    return Result<DesktopEntry>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool isAsciiLetter(char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

[[nodiscard]] bool isAsciiDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool isKeyCharacter(char character) noexcept {
    return isAsciiLetter(character) || isAsciiDigit(character) || character == '-';
}

[[nodiscard]] bool validLocaleComponent(std::string_view component) noexcept {
    return !component.empty() && std::ranges::all_of(component, [](char character) {
        return isAsciiLetter(character) || isAsciiDigit(character) || character == '-';
    });
}

[[nodiscard]] bool validLocaleTag(std::string_view locale) noexcept {
    if (locale.empty()) {
        return false;
    }
    const auto at = locale.find('@');
    if (at != std::string_view::npos && locale.find('@', at + 1U) != std::string_view::npos) {
        return false;
    }
    const auto dot = locale.find('.');
    if (dot != std::string_view::npos && locale.find('.', dot + 1U) != std::string_view::npos) {
        return false;
    }
    if (at != std::string_view::npos && dot != std::string_view::npos && at < dot) {
        return false;
    }
    const auto baseEnd = std::min(at == std::string_view::npos ? locale.size() : at,
                                  dot == std::string_view::npos ? locale.size() : dot);
    const auto base = locale.substr(0U, baseEnd);
    const auto modifier =
        at == std::string_view::npos ? std::string_view{} : locale.substr(at + 1U);
    if (at != std::string_view::npos && !validLocaleComponent(modifier)) {
        return false;
    }
    const auto underscore = base.find('_');
    if (underscore != std::string_view::npos &&
        base.find('_', underscore + 1U) != std::string_view::npos) {
        return false;
    }
    const auto language = base.substr(0U, underscore);
    const auto country =
        underscore == std::string_view::npos ? std::string_view{} : base.substr(underscore + 1U);
    const auto encoding =
        dot == std::string_view::npos
            ? std::string_view{}
            : locale.substr(dot + 1U,
                            (at == std::string_view::npos ? locale.size() : at) - dot - 1U);
    return validLocaleComponent(language) &&
           (underscore == std::string_view::npos || validLocaleComponent(country)) &&
           (dot == std::string_view::npos || validLocaleComponent(encoding));
}

[[nodiscard]] std::string canonicalLocaleTag(std::string_view locale) {
    const auto at = locale.find('@');
    const auto dot = locale.find('.');
    const auto baseEnd = std::min(at == std::string_view::npos ? locale.size() : at,
                                  dot == std::string_view::npos ? locale.size() : dot);
    std::string canonical{locale.substr(0U, baseEnd)};
    if (at != std::string_view::npos) {
        canonical.push_back('@');
        canonical.append(locale.substr(at + 1U));
    }
    return canonical;
}

[[nodiscard]] std::optional<KeyParts> splitKey(std::string_view key) noexcept {
    if (key.empty()) {
        return std::nullopt;
    }
    const auto bracket = key.find('[');
    const auto base = key.substr(0U, bracket);
    if (base.empty() || !std::ranges::all_of(base, isKeyCharacter)) {
        return std::nullopt;
    }
    if (bracket == std::string_view::npos) {
        return KeyParts{base, std::nullopt};
    }
    if (key.back() != ']' || key.find('[', bracket + 1U) != std::string_view::npos ||
        key.find(']', bracket + 1U) != key.size() - 1U) {
        return std::nullopt;
    }
    const auto locale = key.substr(bracket + 1U, key.size() - bracket - 2U);
    if (!validLocaleTag(locale)) {
        return std::nullopt;
    }
    return KeyParts{base, canonicalLocaleTag(locale)};
}

[[nodiscard]] Utf8Status validateUtf8(std::string_view value, std::size_t maximumCodepoints) {
    std::size_t codepoints = 0U;
    for (std::size_t index = 0U; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 0U;
        std::uint32_t codepoint = 0U;
        if (lead <= 0x7FU) {
            width = 1U;
            codepoint = lead;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            width = 2U;
            codepoint = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            width = 3U;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            width = 4U;
            codepoint = lead & 0x07U;
        } else {
            return Utf8Status::invalid;
        }
        if (index + width > value.size()) {
            return Utf8Status::invalid;
        }
        for (std::size_t continuation = 1U; continuation < width; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return Utf8Status::invalid;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((width == 3U && codepoint < 0x800U) || (width == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return Utf8Status::invalid;
        }
        ++codepoints;
        if (codepoints > maximumCodepoints) {
            return Utf8Status::tooManyCodepoints;
        }
        index += width;
    }
    return Utf8Status::valid;
}

[[nodiscard]] bool hasUnsafeControl(std::string_view value) noexcept {
    for (std::size_t index = 0U; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 1U;
        std::uint32_t codepoint = lead;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            width = 2U;
            codepoint = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            width = 3U;
            codepoint = lead & 0x0FU;
        } else if (lead >= 0xF0U) {
            width = 4U;
            codepoint = lead & 0x07U;
        }
        for (std::size_t continuation = 1U; continuation < width; ++continuation) {
            codepoint = (codepoint << 6U) |
                        (static_cast<unsigned char>(value[index + continuation]) & 0x3FU);
        }
        if (codepoint < 0x20U || (codepoint >= 0x7FU && codepoint <= 0x9FU)) {
            return true;
        }
        index += width;
    }
    return false;
}

[[nodiscard]] Result<std::string> decodeString(std::string_view value,
                                               bool allowEscapedSemicolon = false) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (value[index] != '\\') {
            decoded.push_back(value[index]);
            continue;
        }
        if (++index >= value.size()) {
            return Result<std::string>::failure(
                {ErrorCode::syntax_error, "A desktop-entry value has an invalid escape sequence.",
                 "Use only Desktop Entry Specification escape sequences."});
        }
        switch (value[index]) {
        case 's':
            decoded.push_back(' ');
            break;
        case 'n':
            decoded.push_back('\n');
            break;
        case 't':
            decoded.push_back('\t');
            break;
        case 'r':
            decoded.push_back('\r');
            break;
        case '\\':
            decoded.push_back('\\');
            break;
        case ';':
            if (!allowEscapedSemicolon) {
                return Result<std::string>::failure(
                    {ErrorCode::syntax_error,
                     "A desktop-entry value has an invalid escape sequence.",
                     "Use escaped semicolons only in string-list values."});
            }
            decoded.push_back(';');
            break;
        default:
            return Result<std::string>::failure(
                {ErrorCode::syntax_error, "A desktop-entry value has an invalid escape sequence.",
                 "Use only Desktop Entry Specification escape sequences."});
        }
    }
    return Result<std::string>::success(std::move(decoded));
}

[[nodiscard]] Result<std::vector<std::string>> decodeList(std::string_view value) {
    std::vector<std::string> items;
    if (value.empty()) {
        return Result<std::vector<std::string>>::success(std::move(items));
    }
    std::string encodedItem;
    encodedItem.reserve(value.size());
    bool escaped = false;
    for (char character : value) {
        if (escaped) {
            encodedItem.push_back('\\');
            encodedItem.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character != ';') {
            encodedItem.push_back(character);
            continue;
        }
        if (encodedItem.empty()) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::validation_error, "A desktop-entry string list contains an empty item.",
                 "Remove empty items while retaining an optional trailing semicolon."});
        }
        auto decoded = decodeString(encodedItem, true);
        if (!decoded) {
            return Result<std::vector<std::string>>::failure(std::move(decoded).error());
        }
        items.push_back(std::move(decoded).value());
        if (items.size() > maximumDesktopEntryListItems) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::too_large, "A desktop-entry string list has too many items.",
                 "Use a smaller bounded string list."});
        }
        encodedItem.clear();
    }
    if (escaped) {
        return Result<std::vector<std::string>>::failure(
            {ErrorCode::syntax_error, "A desktop-entry value has an invalid escape sequence.",
             "Use only Desktop Entry Specification escape sequences."});
    }
    if (!encodedItem.empty()) {
        auto decoded = decodeString(encodedItem, true);
        if (!decoded) {
            return Result<std::vector<std::string>>::failure(std::move(decoded).error());
        }
        items.push_back(std::move(decoded).value());
        if (items.size() > maximumDesktopEntryListItems) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::too_large, "A desktop-entry string list has too many items.",
                 "Use a smaller bounded string list."});
        }
    }
    return Result<std::vector<std::string>>::success(std::move(items));
}

[[nodiscard]] Result<bool> decodeBoolean(std::string_view value) {
    if (value == "true") {
        return Result<bool>::success(true);
    }
    if (value == "false") {
        return Result<bool>::success(false);
    }
    return Result<bool>::failure({ErrorCode::validation_error,
                                  "A desktop-entry boolean value is invalid.",
                                  "Use exactly true or false."});
}

[[nodiscard]] std::vector<std::string> localeFallbacks(std::string_view locale) {
    if (locale.empty() || locale == "C" || locale == "POSIX") {
        return {};
    }
    const auto at = locale.find('@');
    const auto dot = locale.find('.');
    const auto baseEnd = std::min(at == std::string_view::npos ? locale.size() : at,
                                  dot == std::string_view::npos ? locale.size() : dot);
    const auto base = locale.substr(0U, baseEnd);
    const auto modifier =
        at == std::string_view::npos ? std::string_view{} : locale.substr(at + 1U);
    const auto underscore = base.find('_');
    const auto language = base.substr(0U, underscore);
    const auto country =
        underscore == std::string_view::npos ? std::string_view{} : base.substr(underscore + 1U);

    if (language == "C" || language == "POSIX") {
        return {};
    }
    std::vector<std::string> fallbacks;
    if (!country.empty() && !modifier.empty()) {
        fallbacks.emplace_back(std::string{language} + "_" + std::string{country} + "@" +
                               std::string{modifier});
    }
    if (!country.empty()) {
        fallbacks.emplace_back(std::string{language} + "_" + std::string{country});
    }
    if (!modifier.empty()) {
        fallbacks.emplace_back(std::string{language} + "@" + std::string{modifier});
    }
    fallbacks.emplace_back(language);
    return fallbacks;
}

[[nodiscard]] bool validMessagesLocale(std::string_view locale) noexcept {
    if (locale.empty() || locale == "C" || locale == "POSIX") {
        return true;
    }
    const auto at = locale.find('@');
    if (at != std::string_view::npos && locale.find('@', at + 1U) != std::string_view::npos) {
        return false;
    }
    const auto dot = locale.find('.');
    if (dot != std::string_view::npos && locale.find('.', dot + 1U) != std::string_view::npos) {
        return false;
    }
    if (at != std::string_view::npos && dot != std::string_view::npos && at < dot) {
        return false;
    }
    const auto baseEnd = std::min(at == std::string_view::npos ? locale.size() : at,
                                  dot == std::string_view::npos ? locale.size() : dot);
    const auto base = locale.substr(0U, baseEnd);
    const auto underscore = base.find('_');
    if (underscore != std::string_view::npos &&
        base.find('_', underscore + 1U) != std::string_view::npos) {
        return false;
    }
    const auto language = base.substr(0U, underscore);
    const auto country =
        underscore == std::string_view::npos ? std::string_view{} : base.substr(underscore + 1U);
    const auto encoding =
        dot == std::string_view::npos
            ? std::string_view{}
            : locale.substr(dot + 1U,
                            (at == std::string_view::npos ? locale.size() : at) - dot - 1U);
    const auto modifier =
        at == std::string_view::npos ? std::string_view{} : locale.substr(at + 1U);
    return validLocaleComponent(language) &&
           (underscore == std::string_view::npos || validLocaleComponent(country)) &&
           (dot == std::string_view::npos || validLocaleComponent(encoding)) &&
           (at == std::string_view::npos || validLocaleComponent(modifier));
}

template <typename Value>
[[nodiscard]] std::optional<Value> selectLocalized(const std::map<std::string, Value> &values,
                                                   const std::vector<std::string> &fallbacks) {
    for (const auto &locale : fallbacks) {
        if (const auto found = values.find(locale); found != values.end()) {
            return found->second;
        }
    }
    if (const auto base = values.find(""); base != values.end()) {
        return base->second;
    }
    return std::nullopt;
}

[[nodiscard]] bool isLocalizedKey(std::string_view key) noexcept {
    constexpr std::array keys{"Name", "GenericName", "Comment", "Icon", "Keywords"};
    return std::ranges::find(keys, key) != keys.end();
}

[[nodiscard]] bool isKnownIgnoredKey(std::string_view key) noexcept {
    constexpr std::array keys{"Version",
                              "Actions",
                              "MimeType",
                              "Implements",
                              "StartupNotify",
                              "StartupWMClass",
                              "PrefersNonDefaultGPU",
                              "SingleMainWindow",
                              "URL"};
    return std::ranges::find(keys, key) != keys.end();
}

[[nodiscard]] bool isKnownKey(std::string_view key) noexcept {
    constexpr std::array keys{"Type",    "Name",   "GenericName", "NoDisplay", "Comment",
                              "Icon",    "Hidden", "OnlyShowIn",  "NotShowIn", "DBusActivatable",
                              "TryExec", "Exec",   "Path",        "Terminal",  "Categories",
                              "Keywords"};
    return std::ranges::find(keys, key) != keys.end() || isKnownIgnoredKey(key);
}

[[nodiscard]] Result<void> validateIgnoredValue(std::string_view key, std::string_view value) {
    if (key == "Version") {
        auto decoded = decodeString(value);
        if (!decoded) {
            return Result<void>::failure(std::move(decoded).error());
        }
        constexpr std::array supported{"1.0", "1.1", "1.2", "1.3", "1.4", "1.5"};
        if (std::ranges::find(supported, decoded.value()) == supported.end()) {
            return Result<void>::failure(
                {ErrorCode::unsupported, "The desktop-entry Version is not supported.",
                 "Use a Desktop Entry Specification version from 1.0 through 1.5."});
        }
        return Result<void>::success();
    }
    if (key == "Actions" || key == "MimeType" || key == "Implements") {
        auto decoded = decodeList(value);
        return decoded ? Result<void>::success()
                       : Result<void>::failure(std::move(decoded).error());
    }
    if (key == "StartupNotify" || key == "PrefersNonDefaultGPU" || key == "SingleMainWindow") {
        auto decoded = decodeBoolean(value);
        return decoded ? Result<void>::success()
                       : Result<void>::failure(std::move(decoded).error());
    }
    auto decoded = decodeString(value);
    return decoded ? Result<void>::success() : Result<void>::failure(std::move(decoded).error());
}

[[nodiscard]] Result<void> assignKnownField(ParsedFields &fields, const KeyParts &key,
                                            std::string_view value) {
    const auto locale = key.locale.value_or(std::string{});
    if (key.locale && !isLocalizedKey(key.base)) {
        return Result<void>::failure({ErrorCode::validation_error,
                                      "A non-localizable desktop-entry key has a locale suffix.",
                                      "Remove the locale suffix from this key."});
    }
    if (key.base == "Keywords" || key.base == "Categories" || key.base == "OnlyShowIn" ||
        key.base == "NotShowIn") {
        auto decoded = decodeList(value);
        if (!decoded) {
            return Result<void>::failure(std::move(decoded).error());
        }
        if (key.base == "Keywords") {
            if (!fields.keywords.emplace(locale, std::move(decoded).value()).second) {
                return Result<void>::failure(
                    {ErrorCode::validation_error,
                     "Localized desktop-entry keys collide after locale canonicalization.",
                     "Define at most one encoding-independent value for each locale."});
            }
        } else if (key.base == "Categories") {
            fields.categories = std::move(decoded).value();
        } else if (key.base == "OnlyShowIn") {
            fields.onlyShowIn = std::move(decoded).value();
        } else {
            fields.notShowIn = std::move(decoded).value();
        }
        return Result<void>::success();
    }
    if (key.base == "Terminal" || key.base == "Hidden" || key.base == "NoDisplay" ||
        key.base == "DBusActivatable") {
        auto decoded = decodeBoolean(value);
        if (!decoded) {
            return Result<void>::failure(std::move(decoded).error());
        }
        if (key.base == "Terminal") {
            fields.terminal = decoded.value();
        } else if (key.base == "Hidden") {
            fields.hidden = decoded.value();
        } else if (key.base == "NoDisplay") {
            fields.noDisplay = decoded.value();
        } else {
            fields.dbusActivatable = decoded.value();
        }
        return Result<void>::success();
    }

    auto decoded = decodeString(value);
    if (!decoded) {
        return Result<void>::failure(std::move(decoded).error());
    }
    auto text = std::move(decoded).value();
    if ((key.base == "Exec" || key.base == "TryExec" || key.base == "Path") &&
        hasUnsafeControl(text)) {
        return Result<void>::failure(
            {ErrorCode::validation_error,
             "A desktop-entry command or path contains unsafe control content.",
             "Remove control characters from Exec, TryExec, and Path values."});
    }
    if (key.base == "Type") {
        fields.type = std::move(text);
    } else if (key.base == "Name") {
        if (!fields.names.emplace(locale, std::move(text)).second) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "Localized desktop-entry keys collide after locale canonicalization.",
                 "Define at most one encoding-independent value for each locale."});
        }
    } else if (key.base == "GenericName") {
        if (!fields.genericNames.emplace(locale, std::move(text)).second) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "Localized desktop-entry keys collide after locale canonicalization.",
                 "Define at most one encoding-independent value for each locale."});
        }
    } else if (key.base == "Comment") {
        if (!fields.comments.emplace(locale, std::move(text)).second) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "Localized desktop-entry keys collide after locale canonicalization.",
                 "Define at most one encoding-independent value for each locale."});
        }
    } else if (key.base == "Icon") {
        if (!fields.icons.emplace(locale, std::move(text)).second) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "Localized desktop-entry keys collide after locale canonicalization.",
                 "Define at most one encoding-independent value for each locale."});
        }
    } else if (key.base == "Exec") {
        fields.exec = std::move(text);
    } else if (key.base == "TryExec") {
        fields.tryExec = std::move(text);
    } else if (key.base == "Path") {
        fields.path = std::move(text);
    }
    return Result<void>::success();
}

} // namespace

Result<DesktopEntry> parseDesktopEntry(std::string_view contents,
                                       const DesktopEntryParseContext &context) {
    if (context.messagesLocale.size() > maximumDesktopEntryLocaleBytes ||
        context.messagesLocale.find('\0') != std::string::npos ||
        validateUtf8(context.messagesLocale, maximumDesktopEntryLocaleBytes) != Utf8Status::valid ||
        !validMessagesLocale(context.messagesLocale)) {
        return failure(ErrorCode::invalid_argument,
                       "The desktop-entry messages locale is invalid or too large.",
                       "Use a bounded POSIX messages-locale identifier.");
    }
    if (contents.size() > maximumDesktopEntryFileBytes) {
        return failure(ErrorCode::too_large, "The desktop-entry document exceeds its byte limit.",
                       "Use a smaller bounded desktop-entry document.");
    }
    const auto fileUtf8 = validateUtf8(contents, maximumDesktopEntryCodepoints);
    if (fileUtf8 == Utf8Status::invalid || contents.find('\0') != std::string_view::npos) {
        return failure(ErrorCode::syntax_error,
                       "The desktop-entry document is not valid NUL-free UTF-8.",
                       "Use a valid UTF-8 desktop-entry document.");
    }
    if (fileUtf8 == Utf8Status::tooManyCodepoints) {
        return failure(ErrorCode::too_large,
                       "The desktop-entry document exceeds its codepoint limit.",
                       "Use a smaller bounded desktop-entry document.");
    }

    ParsedFields fields;
    std::unordered_set<std::string> groups;
    std::unordered_set<std::string> groupKeys;
    std::unordered_set<std::string> canonicalGroupKeys;
    std::string currentGroup;
    bool sawFirstGroup = false;
    std::size_t lineCount = 0U;
    std::size_t groupCount = 0U;
    std::size_t entryCount = 0U;
    std::size_t offset = 0U;

    while (offset <= contents.size()) {
        if (++lineCount > maximumDesktopEntryLines) {
            return failure(ErrorCode::too_large,
                           "The desktop-entry document contains too many lines.",
                           "Use a smaller bounded desktop-entry document.");
        }
        const auto newline = contents.find('\n', offset);
        const auto end = newline == std::string_view::npos ? contents.size() : newline;
        auto line = contents.substr(offset, end - offset);
        if (newline != std::string_view::npos && !line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        if (line.size() > maximumDesktopEntryLineBytes) {
            return failure(ErrorCode::too_large, "A desktop-entry line exceeds its byte limit.",
                           "Use shorter bounded desktop-entry lines.");
        }
        for (char character : line) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U || byte == 0x7FU) {
                return failure(ErrorCode::syntax_error,
                               "The desktop-entry document contains a literal control character.",
                               "Use valid escaped desktop-entry string content.");
            }
        }

        if (line.empty() || line.front() == '#') {
            // Blank lines and whole-line comments carry no parser state.
        } else if (line.front() == '[') {
            if (line.size() < 3U || line.back() != ']') {
                return failure(ErrorCode::syntax_error,
                               "A desktop-entry group header is malformed.",
                               "Use a nonempty group name enclosed in brackets.");
            }
            const auto group = line.substr(1U, line.size() - 2U);
            if (group.find('[') != std::string_view::npos ||
                group.find(']') != std::string_view::npos) {
                return failure(ErrorCode::syntax_error,
                               "A desktop-entry group header is malformed.",
                               "Use a nonempty group name enclosed in brackets.");
            }
            if (!sawFirstGroup && group != desktopEntryGroup) {
                return failure(ErrorCode::validation_error,
                               "The first desktop-entry group is not Desktop Entry.",
                               "Place [Desktop Entry] before every other group.");
            }
            sawFirstGroup = true;
            if (++groupCount > maximumDesktopEntryGroups) {
                return failure(ErrorCode::too_large,
                               "The desktop-entry document contains too many groups.",
                               "Use fewer bounded desktop-entry groups.");
            }
            if (!groups.emplace(group).second) {
                return failure(ErrorCode::validation_error,
                               "The desktop-entry document contains a duplicate group.",
                               "Define each group exactly once.");
            }
            currentGroup.assign(group);
            groupKeys.clear();
            canonicalGroupKeys.clear();
        } else {
            if (!sawFirstGroup || currentGroup.empty()) {
                return failure(
                    ErrorCode::syntax_error, "A desktop-entry key appears outside a group.",
                    "Place key-value entries inside [Desktop Entry] or an extension group.");
            }
            if (++entryCount > maximumDesktopEntryEntries) {
                return failure(ErrorCode::too_large,
                               "The desktop-entry document contains too many entries.",
                               "Use fewer bounded desktop-entry entries.");
            }
            const auto equals = line.find('=');
            if (equals == std::string_view::npos || equals == 0U) {
                return failure(ErrorCode::syntax_error,
                               "A desktop-entry key-value line is malformed.",
                               "Use Key=Value syntax without key whitespace.");
            }
            auto keyEnd = equals;
            while (keyEnd > 0U && line[keyEnd - 1U] == ' ') {
                --keyEnd;
            }
            auto valueStart = equals + 1U;
            while (valueStart < line.size() && line[valueStart] == ' ') {
                ++valueStart;
            }
            const auto rawKey = line.substr(0U, keyEnd);
            const auto value = line.substr(valueStart);
            if (rawKey.size() > maximumDesktopEntryKeyBytes ||
                value.size() > maximumDesktopEntryValueBytes) {
                return failure(ErrorCode::too_large,
                               "A desktop-entry key or value exceeds its byte limit.",
                               "Use smaller bounded desktop-entry keys and values.");
            }
            if (validateUtf8(value, maximumDesktopEntryValueCodepoints) ==
                Utf8Status::tooManyCodepoints) {
                return failure(ErrorCode::too_large,
                               "A desktop-entry value exceeds its codepoint limit.",
                               "Use a smaller bounded desktop-entry value.");
            }
            const auto key = splitKey(rawKey);
            if (!key) {
                return failure(ErrorCode::syntax_error, "A desktop-entry key is malformed.",
                               "Use a valid ASCII key and locale suffix.");
            }
            if (!groupKeys.emplace(rawKey).second) {
                return failure(ErrorCode::validation_error,
                               "A desktop-entry group contains a duplicate exact key.",
                               "Define each exact key at most once per group.");
            }
            std::string canonicalKey{key->base};
            if (key->locale) {
                canonicalKey.push_back('[');
                canonicalKey.append(*key->locale);
                canonicalKey.push_back(']');
            }
            if (!canonicalGroupKeys.emplace(std::move(canonicalKey)).second) {
                return failure(
                    ErrorCode::validation_error,
                    "Localized desktop-entry keys collide after locale canonicalization.",
                    "Define at most one encoding-independent value for each locale.");
            }
            if (currentGroup == desktopEntryGroup) {
                if (!isKnownKey(key->base)) {
                    if (!key->base.starts_with("X-")) {
                        return failure(
                            ErrorCode::unsupported,
                            "The Desktop Entry group contains an unsupported bare key.",
                            "Use a Desktop Entry Specification 1.5 key or an X-* extension key.");
                    }
                } else if (isKnownIgnoredKey(key->base)) {
                    if (key->locale) {
                        return failure(ErrorCode::validation_error,
                                       "A non-localizable desktop-entry key has a locale suffix.",
                                       "Remove the locale suffix from this key.");
                    }
                    auto validated = validateIgnoredValue(key->base, value);
                    if (!validated) {
                        return Result<DesktopEntry>::failure(std::move(validated).error());
                    }
                } else {
                    auto assigned = assignKnownField(fields, *key, value);
                    if (!assigned) {
                        return Result<DesktopEntry>::failure(std::move(assigned).error());
                    }
                }
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1U;
    }

    if (!sawFirstGroup) {
        return failure(ErrorCode::validation_error,
                       "The desktop-entry document has no Desktop Entry group.",
                       "Add [Desktop Entry] as the first group.");
    }
    if (fields.type && *fields.type != "Application") {
        return failure(ErrorCode::unsupported, "The desktop-entry Type is not Application.",
                       "Use an Application desktop entry for launcher discovery.");
    }
    const auto localizedWithoutBase = [](const auto &values) {
        return !values.empty() && !values.contains("");
    };
    if (localizedWithoutBase(fields.names) || localizedWithoutBase(fields.genericNames) ||
        localizedWithoutBase(fields.comments) || localizedWithoutBase(fields.icons) ||
        localizedWithoutBase(fields.keywords)) {
        return failure(ErrorCode::validation_error,
                       "A localized desktop-entry key is missing its unlocalized base value.",
                       "Add an unlocalized base value for every localized key family.");
    }

    const auto fallbacks = localeFallbacks(context.messagesLocale);
    DesktopEntry entry;
    entry.name = selectLocalized(fields.names, fallbacks);
    entry.genericName = selectLocalized(fields.genericNames, fallbacks);
    entry.comment = selectLocalized(fields.comments, fallbacks);
    entry.icon = selectLocalized(fields.icons, fallbacks);
    entry.keywords =
        selectLocalized(fields.keywords, fallbacks).value_or(std::vector<std::string>{});
    entry.categories = std::move(fields.categories);
    entry.exec = std::move(fields.exec);
    entry.tryExec = std::move(fields.tryExec);
    entry.path = std::move(fields.path);
    entry.terminal = fields.terminal;
    entry.hidden = fields.hidden;
    entry.noDisplay = fields.noDisplay;
    entry.dbusActivatable = fields.dbusActivatable;
    entry.onlyShowIn = std::move(fields.onlyShowIn);
    entry.notShowIn = std::move(fields.notShowIn);

    if (entry.onlyShowIn && entry.notShowIn) {
        for (const auto &desktop : *entry.onlyShowIn) {
            if (std::ranges::find(*entry.notShowIn, desktop) != entry.notShowIn->end()) {
                return failure(ErrorCode::validation_error,
                               "OnlyShowIn and NotShowIn contain an overlapping desktop name.",
                               "Keep each desktop name in at most one visibility list.");
            }
        }
    }
    if (entry.hidden) {
        return Result<DesktopEntry>::success(std::move(entry));
    }
    if (!fields.type) {
        return failure(ErrorCode::validation_error,
                       "A launchable desktop entry is missing Type=Application.",
                       "Add Type=Application to the Desktop Entry group.");
    }
    const auto baseName = fields.names.find("");
    if (baseName == fields.names.end() || baseName->second.empty() || !entry.name ||
        entry.name->empty()) {
        return failure(ErrorCode::validation_error,
                       "A launchable desktop entry is missing a nonempty base Name.",
                       "Add a nonempty unlocalized Name value.");
    }
    if ((!entry.exec || entry.exec->empty()) && !entry.dbusActivatable) {
        return failure(ErrorCode::validation_error,
                       "A launchable desktop entry has neither Exec nor D-Bus activation.",
                       "Add a nonempty Exec value or set DBusActivatable=true.");
    }
    if (entry.exec && entry.exec->empty()) {
        return failure(ErrorCode::validation_error, "A desktop-entry Exec value is empty.",
                       "Use a nonempty Exec value or omit it for D-Bus activation.");
    }
    return Result<DesktopEntry>::success(std::move(entry));
}

} // namespace prismdrake::launcher
