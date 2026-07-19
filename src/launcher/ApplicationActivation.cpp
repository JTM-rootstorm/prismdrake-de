#include "ApplicationActivation.hpp"

#include "DesktopEntryParser.hpp"

#include <algorithm>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] bool asciiLetter(char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

[[nodiscard]] bool asciiDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool validBusElement(std::string_view element) noexcept {
    return !element.empty() && !asciiDigit(element.front()) &&
           std::ranges::all_of(element, [](char character) {
               return asciiLetter(character) || asciiDigit(character) || character == '_' ||
                      character == '-';
           });
}

[[nodiscard]] bool validBusName(std::string_view name) noexcept {
    if (name.empty() || name.size() > maximumDbusApplicationNameBytes ||
        name.find('.') == std::string_view::npos) {
        return false;
    }
    std::size_t offset = 0U;
    while (offset <= name.size()) {
        const auto separator = name.find('.', offset);
        const auto end = separator == std::string_view::npos ? name.size() : separator;
        if (!validBusElement(name.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return true;
}

[[nodiscard]] bool validActionId(std::string_view identifier) noexcept {
    return !identifier.empty() && identifier.size() <= maximumDesktopActionIdBytes &&
           std::ranges::all_of(identifier, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte >= 0x20U && byte != 0x7fU && character != '\0';
           });
}

} // namespace

Result<DbusApplicationIdentity> deriveDbusApplicationIdentity(const DesktopFileId &desktopFileId) {
    constexpr std::string_view suffix = ".desktop";
    const auto &identifier = desktopFileId.value();
    if (!identifier.ends_with(suffix)) {
        return Result<DbusApplicationIdentity>::failure(
            {ErrorCode::unsupported, "The desktop-file identifier cannot use D-Bus activation.",
             "Use a valid reverse-domain desktop-file identifier ending in .desktop."});
    }
    const std::string serviceName{identifier.substr(0U, identifier.size() - suffix.size())};
    if (!validBusName(serviceName)) {
        return Result<DbusApplicationIdentity>::failure(
            {ErrorCode::unsupported, "The desktop-file identifier is not a valid D-Bus name.",
             "Use a valid reverse-domain desktop-file identifier for D-Bus activation."});
    }
    std::string objectPath{"/"};
    objectPath.reserve(serviceName.size() + 1U);
    for (const char character : serviceName) {
        if (character == '.') {
            objectPath.push_back('/');
        } else if (character == '-') {
            objectPath.push_back('_');
        } else {
            objectPath.push_back(character);
        }
    }
    return Result<DbusApplicationIdentity>::success(
        DbusApplicationIdentity{serviceName, std::move(objectPath)});
}

Result<std::optional<std::string>>
validateDbusDesktopAction(std::optional<std::string_view> actionId) {
    if (!actionId) {
        return Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (!validActionId(*actionId)) {
        return Result<std::optional<std::string>>::failure(
            {ErrorCode::validation_error, "The desktop action identifier is invalid.",
             "Use a nonempty bounded desktop action identifier."});
    }
    return Result<std::optional<std::string>>::success(std::string{*actionId});
}

} // namespace prismdrake::launcher
