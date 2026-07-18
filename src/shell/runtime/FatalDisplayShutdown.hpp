#pragma once

#include "Result.hpp"

#include <functional>
#include <memory>

namespace prismdrake::shell::runtime {

/// Owner-thread gate for the process-fatal X11 transport boundary.
///
/// Task and panel X11 adapters share one instance. The first reported transport loss invokes the
/// injected process-shutdown callback; duplicate or reentrant reports are ignored. This gate does
/// not attempt to recover a Qt platform connection after the display server disappears.
class FatalDisplayShutdown final {
  public:
    using ShutdownCallback = std::function<void(const foundation::Error &)>;

    [[nodiscard]] static foundation::Result<std::unique_ptr<FatalDisplayShutdown>>
    create(ShutdownCallback shutdown);

    FatalDisplayShutdown(const FatalDisplayShutdown &) = delete;
    FatalDisplayShutdown &operator=(const FatalDisplayShutdown &) = delete;

    void request(const foundation::Error &error);

    [[nodiscard]] bool requested() const noexcept { return requested_; }

  private:
    explicit FatalDisplayShutdown(ShutdownCallback shutdown);

    ShutdownCallback shutdown_;
    bool requested_{false};
};

} // namespace prismdrake::shell::runtime
