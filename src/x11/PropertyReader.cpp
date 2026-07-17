#include "PropertyReader.hpp"

#include "X11Connection.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

using PropertyReply = std::unique_ptr<xcb_get_property_reply_t, ReplyDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, ReplyDeleter>;

[[nodiscard]] Result<PropertyValue> invalidSpec() {
    return Result<PropertyValue>::failure(
        {ErrorCode::invalid_argument, "The X11 property bounds or expected format are invalid.",
         "Use a fixed atom type, supported format, and positive bounded item and byte limits."});
}

[[nodiscard]] Result<PropertyValue> mismatchedConnection() {
    return Result<PropertyValue>::failure(
        {ErrorCode::invalid_argument, "The X11 atom cache belongs to another connection.",
         "Create and use the atom cache with the same live X11 connection."});
}

[[nodiscard]] Result<PropertyValue> invalidReply(std::string message) {
    return Result<PropertyValue>::failure(
        {ErrorCode::validation_error, std::move(message),
         "Ignore the untrusted property value and retain the previous valid state."});
}

[[nodiscard]] std::size_t itemWidth(PropertyFormat format) noexcept {
    switch (format) {
    case PropertyFormat::bits_8:
        return 1U;
    case PropertyFormat::bits_16:
        return 2U;
    case PropertyFormat::bits_32:
        return 4U;
    }
    return 0U;
}

[[nodiscard]] bool validSpec(const PropertySpec &spec, AtomId expectedType) noexcept {
    return expectedType.value() != 0U && itemWidth(spec.expectedFormat) != 0U &&
           spec.maximumItems > 0U && spec.maximumItems <= maximumPropertyItems &&
           spec.maximumBytes > 0U && spec.maximumBytes <= maximumPropertyBytes;
}

} // namespace

Result<std::vector<std::uint32_t>> PropertyValue::uint32Items() const {
    if (format_ != PropertyFormat::bits_32 ||
        bytes_.size() != item_count_ * sizeof(std::uint32_t)) {
        return Result<std::vector<std::uint32_t>>::failure(
            {ErrorCode::invalid_argument, "The X11 property is not a valid format-32 value.",
             "Request uint32 items only from a validated format-32 property."});
    }

    std::vector<std::uint32_t> items(item_count_);
    if (!bytes_.empty()) {
        std::memcpy(items.data(), bytes_.data(), bytes_.size());
    }
    return Result<std::vector<std::uint32_t>>::success(std::move(items));
}

Result<PropertyValue> validatePropertyReply(PropertyReplyView reply, AtomId expectedType,
                                            const PropertySpec &spec) {
    if (!validSpec(spec, expectedType)) {
        return invalidSpec();
    }
    if (!reply.type) {
        return Result<PropertyValue>::failure({ErrorCode::not_found,
                                               "The requested X11 property is absent.",
                                               "Use the documented unavailable-feature fallback."});
    }
    if (reply.type.value() != expectedType ||
        reply.format != static_cast<std::uint8_t>(spec.expectedFormat)) {
        return invalidReply("The X11 property reply has an unexpected type or format.");
    }
    if (reply.bytesAfter != 0U) {
        return invalidReply("The X11 property reply was truncated.");
    }

    const auto width = itemWidth(spec.expectedFormat);
    if (reply.itemCount > spec.maximumItems ||
        reply.itemCount > std::numeric_limits<std::size_t>::max() / width) {
        return Result<PropertyValue>::failure(
            {ErrorCode::too_large, "The X11 property reply exceeds its item bound.",
             "Ignore the oversized property and retain the previous valid state."});
    }
    const auto expectedBytes = static_cast<std::size_t>(reply.itemCount) * width;
    if (expectedBytes != reply.value.size()) {
        return invalidReply("The X11 property reply length is inconsistent with its format.");
    }
    if (expectedBytes > spec.maximumBytes || expectedBytes > maximumPropertyBytes) {
        return Result<PropertyValue>::failure(
            {ErrorCode::too_large, "The X11 property reply exceeds its byte bound.",
             "Ignore the oversized property and retain the previous valid state."});
    }

    return Result<PropertyValue>::success(PropertyValue{
        spec.expectedFormat,
        reply.itemCount,
        std::vector<std::byte>{reply.value.begin(), reply.value.end()},
    });
}

Result<PropertyValue> PropertyReader::read(X11Connection &connection, const AtomCache &atoms,
                                           WindowId window, AtomName property,
                                           const PropertySpec &spec) {
    if (!atoms.belongsTo(connection.identity())) {
        return mismatchedConnection();
    }
    const auto expectedType = atoms.atom(spec.expectedType);
    const auto propertyAtom = atoms.atom(property);
    if (!expectedType || !propertyAtom || !validSpec(spec, expectedType.value())) {
        return invalidSpec();
    }

    const auto width = itemWidth(spec.expectedFormat);
    const auto maximumItemBytes = spec.maximumItems > maximumPropertyBytes / width
                                      ? maximumPropertyBytes
                                      : spec.maximumItems * width;
    const auto requestedBytes = std::min(spec.maximumBytes, maximumItemBytes);
    const auto requestedUnits = (requestedBytes + 3U) / 4U;
    if (requestedUnits == 0U || requestedUnits > std::numeric_limits<std::uint32_t>::max()) {
        return invalidSpec();
    }

    auto *native = static_cast<xcb_connection_t *>(connection.nativeConnection());
    if (native == nullptr || !connection.healthy()) {
        return Result<PropertyValue>::failure(
            {ErrorCode::io_error, "The X11 connection is unavailable for a property request.",
             "Reconnect to the X11 server and rebuild mirrored state."});
    }

    xcb_generic_error_t *rawError = nullptr;
    PropertyReply reply{xcb_get_property_reply(
        native,
        xcb_get_property(native, 0U, window.value(), propertyAtom->value(), expectedType->value(),
                         0U, static_cast<std::uint32_t>(requestedUnits)),
        &rawError)};
    ProtocolError error{rawError};
    if (!reply || error || xcb_connection_has_error(native) != 0) {
        return Result<PropertyValue>::failure(
            {ErrorCode::io_error, "The X11 property request failed.",
             "Discard the stale window state and retry only after a lifecycle event."});
    }

    const int valueLength = xcb_get_property_value_length(reply.get());
    if (valueLength < 0 || (valueLength > 0 && xcb_get_property_value(reply.get()) == nullptr)) {
        return invalidReply("The X11 property reply payload is unavailable.");
    }
    const auto *value = static_cast<const std::byte *>(xcb_get_property_value(reply.get()));
    const auto byteCount = static_cast<std::size_t>(valueLength);
    std::optional<AtomId> replyType;
    if (reply->type != XCB_ATOM_NONE) {
        auto convertedType = AtomId::fromProtocol(reply->type);
        if (!convertedType) {
            return invalidReply("The X11 property reply type is invalid.");
        }
        replyType = convertedType.value();
    }
    return validatePropertyReply(PropertyReplyView{replyType, reply->format, reply->bytes_after,
                                                   reply->value_len,
                                                   std::span<const std::byte>{value, byteCount}},
                                 expectedType.value(), spec);
}

} // namespace prismdrake::x11
