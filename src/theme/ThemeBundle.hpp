#pragma once

#include "Result.hpp"
#include "Theme.hpp"

#include <filesystem>

namespace prismdrake::theme {

/// The complete fixed set of version-1 packaged theme layers.
struct ThemeBundle final {
    ThemeDocument base;
    ThemeDocument lustre;
    ThemeDocument forge;
    ThemeDocument accessibility;
};

/// Loads the four fixed filenames beneath a distribution-owned theme directory.
[[nodiscard]] foundation::Result<ThemeBundle>
loadPackagedThemeBundle(const std::filesystem::path &themeDirectory);

} // namespace prismdrake::theme
