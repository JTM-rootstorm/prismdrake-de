#pragma once

#include "Result.hpp"
#include "Theme.hpp"

#include <cstddef>
#include <string_view>

namespace prismdrake::theme {

inline constexpr std::size_t maximumThemeDocumentBytes = std::size_t{1024} * 1024U;
inline constexpr std::size_t maximumThemeStringCodePoints = 256U;
inline constexpr std::size_t maximumThemeArrayItems = 64U;
inline constexpr std::size_t maximumThemeObjectEntries = 256U;
inline constexpr std::size_t maximumThemeNodes = 4096U;
inline constexpr std::size_t maximumThemeNesting = 16U;
inline constexpr double maximumThemeMetricValue = 65535.0;

/// Fuzz-friendly, display-free parser for one complete version-1 JSON theme document.
/// Diagnostics contain canonical schema paths and never echo document keys or values.
[[nodiscard]] foundation::Result<ThemeDocument> parseThemeDocumentJson(std::string_view input);

} // namespace prismdrake::theme
