#include "FatalDisplayShutdown.hpp"

#include <new>
#include <utility>

namespace prismdrake::shell::runtime {

FatalDisplayShutdown::FatalDisplayShutdown(ShutdownCallback shutdown)
    : shutdown_(std::move(shutdown)) {}

foundation::Result<std::unique_ptr<FatalDisplayShutdown>>
FatalDisplayShutdown::create(ShutdownCallback shutdown) {
    if (!shutdown) {
        return foundation::Result<std::unique_ptr<FatalDisplayShutdown>>::failure(
            {foundation::ErrorCode::invalid_argument,
             "The fatal-display shutdown callback is missing.",
             "Provide the shell process shutdown callback before observing X11 transports."});
    }

    try {
        return foundation::Result<std::unique_ptr<FatalDisplayShutdown>>::success(
            std::unique_ptr<FatalDisplayShutdown>(new FatalDisplayShutdown(std::move(shutdown))));
    } catch (const std::bad_alloc &) {
        return foundation::Result<std::unique_ptr<FatalDisplayShutdown>>::failure(
            {foundation::ErrorCode::too_large,
             "The fatal-display shutdown gate could not be allocated.",
             "Reduce memory pressure before restarting prismdrake-shell."});
    }
}

void FatalDisplayShutdown::request(const foundation::Error &error) {
    if (requested_) {
        return;
    }
    requested_ = true;
    shutdown_(error);
}

} // namespace prismdrake::shell::runtime
