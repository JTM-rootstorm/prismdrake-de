#pragma once

#include "Result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDesktopFileRelativePathBytes = 4096U;
inline constexpr std::size_t maximumDesktopFilePathComponents = 64U;
inline constexpr std::size_t maximumDesktopFileIdBytes = 4096U;

/// A Desktop Entry Specification file identifier derived from a relative path.
class DesktopFileId final {
  public:
    [[nodiscard]] const std::string &value() const noexcept { return value_; }

    friend bool operator==(const DesktopFileId &, const DesktopFileId &) = default;

  private:
    explicit DesktopFileId(std::string value) : value_(std::move(value)) {}

    std::string value_;

    friend foundation::Result<DesktopFileId> deriveDesktopFileId(std::string_view relativePath);
};

/// Derives an identifier from a bounded path relative to an applications root.
///
/// The caller must already have established the applications root and the
/// relative path beneath it. This function performs no filesystem access or
/// canonicalization.
[[nodiscard]] foundation::Result<DesktopFileId> deriveDesktopFileId(std::string_view relativePath);

} // namespace prismdrake::launcher
