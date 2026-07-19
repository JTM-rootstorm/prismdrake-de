#include "DesktopFileId.hpp"

#include <cstdint>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] Result<DesktopFileId> failure(ErrorCode code, std::string message,
                                            std::string recovery) {
    return Result<DesktopFileId>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool validDesktopFileUtf8(std::string_view value) noexcept {
    std::size_t offset = 0U;
    std::size_t codePoints = 0U;
    while (offset < value.size()) {
        const auto lead = static_cast<std::uint8_t>(value[offset]);
        std::uint32_t codePoint = 0U;
        std::size_t length = 0U;
        if (lead <= 0x7fU) {
            codePoint = lead;
            length = 1U;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            codePoint = lead & 0x1fU;
            length = 2U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            codePoint = lead & 0x0fU;
            length = 3U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            codePoint = lead & 0x07U;
            length = 4U;
        } else {
            return false;
        }
        if (length > value.size() - offset) {
            return false;
        }
        for (std::size_t index = 1U; index < length; ++index) {
            const auto continuation = static_cast<std::uint8_t>(value[offset + index]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (continuation & 0x3fU);
        }
        if ((length == 2U && codePoint < 0x80U) || (length == 3U && codePoint < 0x800U) ||
            (length == 4U && codePoint < 0x10000U) ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU) || codePoint > 0x10ffffU ||
            codePoint <= 0x1fU || codePoint == 0x7fU ||
            (codePoint >= 0x80U && codePoint <= 0x9fU)) {
            return false;
        }
        ++codePoints;
        if (codePoints > maximumDesktopFileIdCodePoints) {
            return false;
        }
        offset += length;
    }
    return true;
}

} // namespace

Result<DesktopFileId> deriveDesktopFileId(std::string_view relativePath) {
    constexpr std::string_view desktopSuffix = ".desktop";
    if (relativePath.size() > maximumDesktopFileRelativePathBytes) {
        return failure(ErrorCode::too_large, "The desktop-file relative path is too large.",
                       "Use a shorter path beneath the applications directory.");
    }
    if (relativePath.empty() || relativePath.front() == '/' || relativePath.back() == '/' ||
        !validDesktopFileUtf8(relativePath)) {
        return failure(ErrorCode::invalid_argument, "The desktop-file relative path is invalid.",
                       "Use a nonempty relative path containing bounded UTF-8 without controls.");
    }
    const auto finalSeparator = relativePath.rfind('/');
    const auto fileName = finalSeparator == std::string_view::npos
                              ? relativePath
                              : relativePath.substr(finalSeparator + 1U);
    if (!fileName.ends_with(desktopSuffix) || fileName.size() == desktopSuffix.size()) {
        return failure(ErrorCode::validation_error,
                       "The desktop-file path does not have the required extension.",
                       "Use a file whose name ends in the exact .desktop extension.");
    }

    std::string identifier;
    identifier.reserve(relativePath.size());
    std::size_t componentCount = 0U;
    std::size_t offset = 0U;
    while (offset < relativePath.size()) {
        if (++componentCount > maximumDesktopFilePathComponents) {
            return failure(ErrorCode::too_large,
                           "The desktop-file relative path has too many components.",
                           "Use a shallower path beneath the applications directory.");
        }

        const auto separator = relativePath.find('/', offset);
        const auto end = separator == std::string_view::npos ? relativePath.size() : separator;
        const auto component = relativePath.substr(offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return failure(ErrorCode::invalid_argument,
                           "The desktop-file relative path has an invalid component.",
                           "Use named path components without dot traversal.");
        }

        if (!identifier.empty()) {
            identifier.push_back('-');
        }
        identifier.append(component);

        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }

    if (identifier.size() > maximumDesktopFileIdBytes) {
        return failure(ErrorCode::too_large, "The desktop-file identifier is too large.",
                       "Use a shorter path beneath the applications directory.");
    }
    return Result<DesktopFileId>::success(DesktopFileId{std::move(identifier)});
}

} // namespace prismdrake::launcher
