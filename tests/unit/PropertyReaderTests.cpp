#include "PropertyReader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;

[[nodiscard]] AtomId atomId(AtomId::Value value) { return AtomId::fromProtocol(value).value(); }

const AtomId expectedCardinal = atomId(41U);
constexpr PropertySpec twoCardinals{AtomName::cardinal, PropertyFormat::bits_32, 2U,
                                    2U * sizeof(std::uint32_t)};

TEST(PropertyReaderTest, CopiesACompleteStrictFormat32Reply) {
    std::array<std::uint32_t, 2U> source{7U, 11U};
    const auto sourceBytes = std::as_bytes(std::span{source});

    auto value = validatePropertyReply({expectedCardinal, 32U, 0U, source.size(), sourceBytes},
                                       expectedCardinal, twoCardinals);

    ASSERT_TRUE(value);
    EXPECT_EQ(value.value().format(), PropertyFormat::bits_32);
    EXPECT_EQ(value.value().itemCount(), 2U);
    EXPECT_EQ(value.value().bytes().size(), 8U);
    source = {0U, 0U};
    const auto items = value.value().uint32Items();
    ASSERT_TRUE(items);
    EXPECT_EQ(items.value(), (std::vector<std::uint32_t>{7U, 11U}));
}

TEST(PropertyReaderTest, DistinguishesAbsentFromMalformedProperties) {
    const auto absent =
        validatePropertyReply({std::nullopt, 0U, 0U, 0U, {}}, expectedCardinal, twoCardinals);
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, ErrorCode::not_found);

    const std::array<std::uint32_t, 1U> item{7U};
    const auto bytes = std::as_bytes(std::span{item});
    const auto wrongType =
        validatePropertyReply({atomId(99U), 32U, 0U, 1U, bytes}, expectedCardinal, twoCardinals);
    ASSERT_FALSE(wrongType);
    EXPECT_EQ(wrongType.error().code, ErrorCode::validation_error);

    const auto wrongFormat = validatePropertyReply({expectedCardinal, 16U, 0U, 2U, bytes},
                                                   expectedCardinal, twoCardinals);
    ASSERT_FALSE(wrongFormat);
    EXPECT_EQ(wrongFormat.error().code, ErrorCode::validation_error);
}

TEST(PropertyReaderTest, RejectsTruncationAndInconsistentLengths) {
    const std::array<std::uint32_t, 1U> item{7U};
    const auto bytes = std::as_bytes(std::span{item});
    const auto truncated = validatePropertyReply({expectedCardinal, 32U, 4U, 1U, bytes},
                                                 expectedCardinal, twoCardinals);
    ASSERT_FALSE(truncated);
    EXPECT_EQ(truncated.error().code, ErrorCode::validation_error);

    const auto inconsistent = validatePropertyReply({expectedCardinal, 32U, 0U, 2U, bytes},
                                                    expectedCardinal, twoCardinals);
    ASSERT_FALSE(inconsistent);
    EXPECT_EQ(inconsistent.error().code, ErrorCode::validation_error);
}

TEST(PropertyReaderTest, EnforcesIndependentItemAndByteBounds) {
    const std::array<std::uint32_t, 2U> items{7U, 11U};
    const auto bytes = std::as_bytes(std::span{items});
    const PropertySpec oneItem{AtomName::cardinal, PropertyFormat::bits_32, 1U, bytes.size()};
    const auto tooMany =
        validatePropertyReply({expectedCardinal, 32U, 0U, 2U, bytes}, expectedCardinal, oneItem);
    ASSERT_FALSE(tooMany);
    EXPECT_EQ(tooMany.error().code, ErrorCode::too_large);

    const PropertySpec fourBytes{AtomName::cardinal, PropertyFormat::bits_32, 2U, 4U};
    const auto tooManyBytes =
        validatePropertyReply({expectedCardinal, 32U, 0U, 2U, bytes}, expectedCardinal, fourBytes);
    ASSERT_FALSE(tooManyBytes);
    EXPECT_EQ(tooManyBytes.error().code, ErrorCode::too_large);
}

TEST(PropertyReaderTest, RejectsInvalidBoundsAndFormats) {
    const PropertySpec zeroItems{AtomName::cardinal, PropertyFormat::bits_32, 0U, 4U};
    EXPECT_FALSE(validatePropertyReply({}, expectedCardinal, zeroItems));

    const PropertySpec zeroBytes{AtomName::cardinal, PropertyFormat::bits_32, 1U, 0U};
    EXPECT_FALSE(validatePropertyReply({}, expectedCardinal, zeroBytes));

    const PropertySpec tooManyItems{AtomName::cardinal, PropertyFormat::bits_32,
                                    maximumPropertyItems + 1U, 4U};
    EXPECT_FALSE(validatePropertyReply({}, expectedCardinal, tooManyItems));

    const PropertySpec tooManyBytes{AtomName::cardinal, PropertyFormat::bits_32, 1U,
                                    maximumPropertyBytes + 1U};
    EXPECT_FALSE(validatePropertyReply({}, expectedCardinal, tooManyBytes));

    const PropertySpec invalidFormat{AtomName::cardinal, static_cast<PropertyFormat>(24U), 1U, 4U};
    EXPECT_FALSE(validatePropertyReply({}, expectedCardinal, invalidFormat));
}

TEST(PropertyReaderTest, RejectsUint32ConversionFromOtherFormats) {
    constexpr PropertySpec bytesSpec{AtomName::string, PropertyFormat::bits_8, 4U, 4U};
    const std::array<std::byte, 4U> bytes{std::byte{'t'}, std::byte{'e'}, std::byte{'s'},
                                          std::byte{'t'}};
    const auto stringAtom = atomId(17U);
    auto value =
        validatePropertyReply({stringAtom, 8U, 0U, bytes.size(), bytes}, stringAtom, bytesSpec);
    ASSERT_TRUE(value);

    const auto converted = value.value().uint32Items();

    ASSERT_FALSE(converted);
    EXPECT_EQ(converted.error().code, ErrorCode::invalid_argument);
}

TEST(PropertyReaderTest, ValidationErrorsNeverReflectPropertyPayloads) {
    constexpr std::string_view sentinel = "private-window-title-sentinel";
    const auto bytes = std::as_bytes(std::span{sentinel.data(), sentinel.size()});
    const auto invalid = validatePropertyReply({expectedCardinal, 8U, 1U, sentinel.size(), bytes},
                                               expectedCardinal, twoCardinals);

    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().message.find(sentinel), std::string::npos);
    EXPECT_EQ(invalid.error().recovery.find(sentinel), std::string::npos);
}

} // namespace
} // namespace prismdrake::x11
