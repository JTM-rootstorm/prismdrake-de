#pragma once

#include "Result.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace prismdrake::foundation {

/// Reads one regular file without allocating or retaining more than maxBytes.
///
/// The returned string is binary-safe. A zero or unrepresentable limit is an
/// invalid argument, and a file containing more than maxBytes is rejected.
[[nodiscard]] Result<std::string> readBoundedFile(const std::filesystem::path &path,
                                                  std::size_t maxBytes);

} // namespace prismdrake::foundation
