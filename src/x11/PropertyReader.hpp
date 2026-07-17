#pragma once

#include "AtomCache.hpp"
#include "Result.hpp"
#include "X11Types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace prismdrake::x11 {

class X11Connection;

inline constexpr std::size_t maximumPropertyBytes = 1024U * 1024U;
inline constexpr std::size_t maximumPropertyItems = maximumPropertyBytes;

enum class PropertyFormat : std::uint8_t {
    bits_8 = 8U,
    bits_16 = 16U,
    bits_32 = 32U,
};

struct PropertySpec final {
    AtomName expectedType;
    PropertyFormat expectedFormat;
    std::size_t maximumItems;
    std::size_t maximumBytes;
};

/// Validated bounded property value copied out of an owned XCB reply.
class PropertyValue final {
  public:
    [[nodiscard]] PropertyFormat format() const noexcept { return format_; }
    [[nodiscard]] std::size_t itemCount() const noexcept { return item_count_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

    /// Copies a format-32 property into naturally aligned host uint32 values.
    [[nodiscard]] foundation::Result<std::vector<std::uint32_t>> uint32Items() const;

  private:
    friend class PropertyReader;
    friend foundation::Result<PropertyValue> validatePropertyReply(struct PropertyReplyView reply,
                                                                   AtomId expectedType,
                                                                   const PropertySpec &spec);

    PropertyValue(PropertyFormat format, std::size_t itemCount, std::vector<std::byte> bytes)
        : format_(format), item_count_(itemCount), bytes_(std::move(bytes)) {}

    PropertyFormat format_;
    std::size_t item_count_;
    std::vector<std::byte> bytes_;
};

/// Protocol-neutral view used to unit-test validation of an untrusted XCB property reply.
struct PropertyReplyView final {
    std::optional<AtomId> type;
    std::uint8_t format;
    std::uint32_t bytesAfter;
    std::uint32_t itemCount;
    std::span<const std::byte> value;
};

/// Validates and copies one already bounded property reply without retaining borrowed memory.
[[nodiscard]] foundation::Result<PropertyValue>
validatePropertyReply(PropertyReplyView reply, AtomId expectedType, const PropertySpec &spec);

/// Bounded strict core-XCB property reader for one connection and its immutable atom cache.
class PropertyReader final {
  public:
    /// Reads with live arguments only and rejects an atom cache from another connection.
    [[nodiscard]] static foundation::Result<PropertyValue> read(X11Connection &connection,
                                                                const AtomCache &atoms,
                                                                WindowId window, AtomName property,
                                                                const PropertySpec &spec);
};

} // namespace prismdrake::x11
