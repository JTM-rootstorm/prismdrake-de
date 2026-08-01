#pragma once

#include "Result.hpp"

#include <cstdint>
#include <optional>

namespace prismdrake::shell::runtime {

/// Serializes checked pre-map task-policy publication for shell-owned singleton windows.
/// Pending native windows must remain alive until a non-stale completion is classified.
class SingletonTaskPolicyGate final {
  public:
    enum class CompletionDisposition : std::uint8_t { stale, ready, discard, fatal };

    [[nodiscard]] foundation::Result<void> begin(std::uint64_t token);
    void invalidate() noexcept;
    void revalidate() noexcept;
    [[nodiscard]] CompletionDisposition complete(std::uint64_t token, bool success,
                                                 bool settingsOwnerAvailable) noexcept;

    [[nodiscard]] bool pending() const noexcept { return token_.has_value(); }
    [[nodiscard]] bool ready() const noexcept { return ready_; }

  private:
    std::optional<std::uint64_t> token_;
    bool invalidated_{false};
    bool ready_{false};
};

} // namespace prismdrake::shell::runtime
