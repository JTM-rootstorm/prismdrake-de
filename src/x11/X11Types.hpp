#pragma once

#include "Result.hpp"

#include <cstdint>

namespace prismdrake::x11 {

/// Explicit non-owning X11 window identifier. Zero is never a valid live window.
class WindowId final {
  public:
    using Value = std::uint32_t;

    [[nodiscard]] static foundation::Result<WindowId> fromProtocol(Value value);

    [[nodiscard]] Value value() const noexcept { return value_; }

    friend bool operator==(const WindowId &, const WindowId &) = default;

  private:
    explicit WindowId(Value value) noexcept : value_(value) {}

    Value value_;
};

struct ScreenInfo final {
    std::uint32_t screenIndex;
    WindowId rootWindow;
    std::uint32_t widthPx;
    std::uint32_t heightPx;

    friend bool operator==(const ScreenInfo &, const ScreenInfo &) = default;
};

} // namespace prismdrake::x11
