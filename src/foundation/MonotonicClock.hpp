#pragma once

#include "Result.hpp"

#include <chrono>
#include <mutex>

namespace prismdrake::foundation {

/// Injectable source of monotonic time for timeouts and bounded retry policy.
class MonotonicClock {
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    virtual ~MonotonicClock() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

/// Production monotonic clock backed by std::chrono::steady_clock.
class SystemMonotonicClock final : public MonotonicClock {
  public:
    [[nodiscard]] TimePoint now() const noexcept override { return Clock::now(); }
};

/// Manually advanced monotonic clock for deterministic tests.
class TestMonotonicClock final : public MonotonicClock {
  public:
    explicit TestMonotonicClock(TimePoint start = TimePoint{}) noexcept : current_(start) {}

    [[nodiscard]] TimePoint now() const noexcept override {
        std::scoped_lock lock(mutex_);
        return current_;
    }

    [[nodiscard]] Result<void> advance(Duration delta) {
        std::scoped_lock lock(mutex_);
        if (delta < Duration::zero()) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "test clock cannot move backward",
                                          "advance the test clock by a non-negative duration"});
        }
        if (current_ > TimePoint::max() - delta) {
            return Result<void>::failure({ErrorCode::too_large, "test clock advance would overflow",
                                          "use a smaller deterministic test duration"});
        }
        current_ += delta;
        return Result<void>::success();
    }

  private:
    mutable std::mutex mutex_;
    TimePoint current_;
};

} // namespace prismdrake::foundation
