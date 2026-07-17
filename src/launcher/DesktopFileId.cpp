#include "DesktopFileId.hpp"

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

} // namespace

Result<DesktopFileId> deriveDesktopFileId(std::string_view relativePath) {
    constexpr std::string_view desktopSuffix = ".desktop";
    if (relativePath.size() > maximumDesktopFileRelativePathBytes) {
        return failure(ErrorCode::too_large, "The desktop-file relative path is too large.",
                       "Use a shorter path beneath the applications directory.");
    }
    if (relativePath.empty() || relativePath.front() == '/' || relativePath.back() == '/' ||
        relativePath.find('\0') != std::string_view::npos) {
        return failure(ErrorCode::invalid_argument, "The desktop-file relative path is invalid.",
                       "Use a nonempty relative path without null bytes.");
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
