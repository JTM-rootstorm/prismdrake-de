#pragma once

#include "Result.hpp"

#include <filesystem>
#include <string_view>

namespace prismdrake::foundation {

struct AtomicWriteOptions {
    /// Exact permissions for a newly created destination. Existing regular
    /// files preserve their current read, write, and execute permissions.
    std::filesystem::perms createPermissions =
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
};

/// Replaces a regular file through a unique temporary file in the same directory.
///
/// The payload is binary-safe. Symbolic-link destinations and non-regular
/// destinations are rejected. Failures before the atomic rename leave the
/// prior destination untouched and remove the temporary file.
[[nodiscard]] Result<void> writeFileAtomically(const std::filesystem::path &destination,
                                               std::string_view payload,
                                               AtomicWriteOptions options = {});

} // namespace prismdrake::foundation
