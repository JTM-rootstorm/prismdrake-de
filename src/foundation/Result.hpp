#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace prismdrake::foundation {

enum class ErrorCode : std::uint8_t {
    invalid_argument,
    invalid_environment,
    not_found,
    permission_denied,
    too_large,
    io_error,
    durability_uncertain,
    unsupported,
    cancelled,
};

/// Bounded actionable failure details for internal foundation operations.
struct Error {
    ErrorCode code;
    std::string message;
    std::string recovery;

    friend bool operator==(const Error &, const Error &) = default;
};

/// Value-or-error result used where C++20 has no standard expected type.
template <typename T> class [[nodiscard]] Result {
  public:
    static Result success(T value) { return Result(std::in_place_index<0>, std::move(value)); }

    static Result failure(Error error) { return Result(std::in_place_index<1>, std::move(error)); }

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T &value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T &value() const & { return std::get<0>(storage_); }
    [[nodiscard]] T &&value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] Error &error() & { return std::get<1>(storage_); }
    [[nodiscard]] const Error &error() const & { return std::get<1>(storage_); }

  private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value &&value)
        : storage_(index, std::forward<Value>(value)) {}

    std::variant<T, Error> storage_;
};

/// Void specialization for operations that report only success or failure.
template <> class [[nodiscard]] Result<void> {
  public:
    static Result success() { return Result(std::in_place_index<0>, std::monostate{}); }
    static Result failure(Error error) { return Result(std::in_place_index<1>, std::move(error)); }

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] Error &error() & { return std::get<1>(storage_); }
    [[nodiscard]] const Error &error() const & { return std::get<1>(storage_); }

  private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value &&value)
        : storage_(index, std::forward<Value>(value)) {}

    std::variant<std::monostate, Error> storage_;
};

} // namespace prismdrake::foundation
