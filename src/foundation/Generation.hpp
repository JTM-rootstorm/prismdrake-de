#pragma once

#include "Result.hpp"

#include <compare>
#include <cstdint>
#include <limits>

namespace prismdrake::foundation {

/// Identifies one complete, published settings or theme snapshot.
class Generation final {
  public:
    using Value = std::uint64_t;

    static constexpr Value unpublishedValue = 0;

    [[nodiscard]] static Generation firstPublished() noexcept { return Generation(1U); }

    [[nodiscard]] static Result<Generation> fromPublished(Value value) {
        if (value == unpublishedValue) {
            return Result<Generation>::failure(
                {ErrorCode::invalid_argument, "generation zero is not published",
                 "publish a complete snapshot before assigning its generation"});
        }
        return Result<Generation>::success(Generation(value));
    }

    [[nodiscard]] Value value() const noexcept { return value_; }

    [[nodiscard]] Result<Generation> next() const {
        if (value_ == std::numeric_limits<Value>::max()) {
            return Result<Generation>::failure(
                {ErrorCode::too_large, "generation counter is exhausted",
                 "stop publication and restart through a reviewed recovery path"});
        }
        return Result<Generation>::success(Generation(value_ + 1));
    }

    friend auto operator<=>(const Generation &, const Generation &) = default;

  private:
    explicit constexpr Generation(Value value) noexcept : value_(value) {}

    Value value_;
};

} // namespace prismdrake::foundation
