#pragma once

#include "IconRequest.hpp"
#include "Result.hpp"

#include <QObject>

#include <cstddef>
#include <functional>
#include <memory>

namespace prismdrake::icons {

inline constexpr std::size_t maximumIconThemeSearchRoots = 128U;
inline constexpr std::size_t maximumIconThemeSearchPathBytes = 64U * 1024U;

/// Qt-bound asynchronous icon adapter. Qt theme availability is captured on the context thread;
/// bounded theme-file discovery and all image decoding run on worker threads.
class IconResolver final {
  public:
    using Completion = std::function<void(IconResolution)>;

    explicit IconResolver(std::size_t maximumOutstandingRequests = 32U);
    ~IconResolver();
    IconResolver(const IconResolver &) = delete;
    IconResolver &operator=(const IconResolver &) = delete;
    IconResolver(IconResolver &&) noexcept;
    IconResolver &operator=(IconResolver &&) noexcept;

    /// Queues one immutable request and invokes completion on context's thread if it remains live.
    [[nodiscard]] foundation::Result<void> resolve(IconRequest request, QObject *context,
                                                   Completion completion) const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace prismdrake::icons
