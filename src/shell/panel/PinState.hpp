#pragma once

#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace prismdrake::shell::panel {

inline constexpr std::size_t maximumPinCount = 256U;
inline constexpr std::size_t maximumPinStateBytes = 2U * 1024U * 1024U;

enum class PinStateLoadStatus : std::uint8_t { loaded, missing, invalidRetained };

struct PinStateSnapshot final {
    std::uint64_t generation;
    std::vector<launcher::DesktopFileId> pins;
    PinStateLoadStatus loadStatus;
};

/// Single-owner private pin persistence with atomic complete-snapshot publication.
class PinStateStore final {
  public:
    [[nodiscard]] static foundation::Result<PinStateStore> open(std::filesystem::path stateFile);

    /// Opens the Accepted pin-state location below the Prismdrake XDG state directory.
    ///
    /// The Prismdrake and shell directories are created when absent and must remain
    /// private, current-user-owned, non-symlink directories.
    [[nodiscard]] static foundation::Result<PinStateStore>
    openInStateDirectory(std::filesystem::path prismdrakeStateDirectory);

    PinStateStore(const PinStateStore &) = delete;
    PinStateStore &operator=(const PinStateStore &) = delete;
    PinStateStore(PinStateStore &&) noexcept = default;
    PinStateStore &operator=(PinStateStore &&) noexcept = default;

    [[nodiscard]] const std::shared_ptr<const PinStateSnapshot> &current() const noexcept {
        return current_;
    }

    /// Replaces the entire ordered set after validation and a successful private atomic write.
    [[nodiscard]] foundation::Result<std::shared_ptr<const PinStateSnapshot>>
    replace(std::vector<launcher::DesktopFileId> pins);

  private:
    PinStateStore(std::filesystem::path stateFile, std::shared_ptr<const PinStateSnapshot> current)
        : state_file_(std::move(stateFile)), current_(std::move(current)) {}

    std::filesystem::path state_file_;
    std::shared_ptr<const PinStateSnapshot> current_;
};

} // namespace prismdrake::shell::panel
