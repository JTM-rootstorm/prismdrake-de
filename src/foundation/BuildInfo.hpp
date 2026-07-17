#pragma once

#include <string_view>

namespace prismdrake::foundation {

/// Returns the tracked Prismdrake product version used for this build.
[[nodiscard]] std::string_view productVersion() noexcept;

/// Reports whether non-production developer overrides were compiled in.
[[nodiscard]] bool developerOverridesEnabled() noexcept;

} // namespace prismdrake::foundation
