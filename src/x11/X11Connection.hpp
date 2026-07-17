#pragma once

#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace prismdrake::x11 {

class AtomCache;
class PropertyReader;

inline constexpr std::size_t maximumDisplayNameBytes = 255U;

/// One owned core-XCB connection to an explicitly selected display.
///
/// The public boundary exposes no raw XCB object and owns no window-manager
/// policy. Construction validates the selected screen and completes one checked
/// server round trip before returning.
class X11Connection final {
  public:
    [[nodiscard]] static foundation::Result<X11Connection> connect(std::string_view display);

    ~X11Connection();
    X11Connection(const X11Connection &) = delete;
    X11Connection &operator=(const X11Connection &) = delete;
    X11Connection(X11Connection &&) noexcept;
    X11Connection &operator=(X11Connection &&) noexcept;

    [[nodiscard]] const ScreenInfo &screen() const noexcept;
    [[nodiscard]] int eventFileDescriptor() const noexcept;
    [[nodiscard]] bool healthy() const noexcept;

  private:
    friend class AtomCache;
    friend class PropertyReader;

    class Implementation;
    using Identity = std::uint64_t;

    explicit X11Connection(std::shared_ptr<Implementation> implementation) noexcept;
    [[nodiscard]] void *nativeConnection() const noexcept;
    [[nodiscard]] Identity identity() const noexcept;

    std::shared_ptr<Implementation> implementation_;
};

/// Opens, verifies, and closes one connection to prove that DISPLAY names a
/// usable X11 server. A window manager or EWMH support is deliberately not
/// required by this transport-level probe.
[[nodiscard]] foundation::Result<ScreenInfo> probeUsableDisplay(std::string_view display);

} // namespace prismdrake::x11
