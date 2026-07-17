#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

[[nodiscard]] WindowMetadataObservation observation(std::uint32_t windowValue = 41U) {
    return WindowMetadataObservation{
        window(windowValue), std::nullopt, std::nullopt, std::nullopt, {}, {},
        std::nullopt,        std::nullopt, std::nullopt, std::nullopt};
}

[[nodiscard]] std::vector<std::uint8_t> wmClass(std::string instance, std::string className) {
    std::vector<std::uint8_t> property;
    property.reserve(instance.size() + className.size() + 2U);
    property.insert(property.end(), instance.begin(), instance.end());
    property.push_back(0U);
    property.insert(property.end(), className.begin(), className.end());
    property.push_back(0U);
    return property;
}

TEST(WindowMetadataTest, MissingOptionalPropertiesProduceBoundedGenericDefaults) {
    const auto decoded = decodeWindowMetadata(observation());

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().window, window(41U));
    EXPECT_EQ(decoded.value().displayTitle, "Untitled Window");
    EXPECT_EQ(decoded.value().identity.source, ApplicationIdentitySource::genericUnknown);
    EXPECT_FALSE(decoded.value().identity.instance);
    EXPECT_FALSE(decoded.value().identity.className);
    EXPECT_EQ(decoded.value().identity.groupingKey, "unknown-application");
    EXPECT_EQ(decoded.value().type, WindowType::normal);
    EXPECT_FALSE(decoded.value().typeWasExplicit);
    EXPECT_TRUE(decoded.value().states.empty());
    EXPECT_FALSE(decoded.value().workspace);
    EXPECT_FALSE(decoded.value().onAllWorkspaces);
    EXPECT_FALSE(decoded.value().minimized);
    EXPECT_FALSE(decoded.value().urgent);
    EXPECT_FALSE(decoded.value().skipTaskbar);
    EXPECT_FALSE(decoded.value().modal);
    EXPECT_FALSE(decoded.value().transientFor);
    EXPECT_TRUE(decoded.value().icons.empty());
}

TEST(WindowMetadataTest, PrefersValidUtf8TitleAndFallsBackToLatin1LegacyTitle) {
    auto preferred = observation();
    preferred.utf8Title = "Prismdrake \xf0\x9f\x90\x89";
    preferred.legacyTitle = "Legacy";
    const auto utf8 = decodeWindowMetadata(preferred);
    ASSERT_TRUE(utf8);
    EXPECT_EQ(utf8.value().displayTitle, preferred.utf8Title.value());

    auto fallback = observation();
    fallback.utf8Title = "";
    fallback.legacyTitle = std::string{"Caf\xe9", 4U};
    const auto legacy = decodeWindowMetadata(fallback);
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy.value().displayTitle, "Caf\xc3\xa9");
}

TEST(WindowMetadataTest, RejectsMalformedAndOversizedTitlesWithoutReflectingContents) {
    auto malformed = observation();
    malformed.utf8Title = std::string{"private\xc0\xaf", 9U};
    const auto invalidUtf8 = decodeWindowMetadata(malformed);
    ASSERT_FALSE(invalidUtf8);
    EXPECT_EQ(invalidUtf8.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(invalidUtf8.error().message.find("private"), std::string::npos);
    EXPECT_EQ(invalidUtf8.error().recovery.find("private"), std::string::npos);

    auto oversized = observation();
    oversized.utf8Title = std::string(maximumWindowTitleBytes + 1U, 's');
    const auto tooLarge = decodeWindowMetadata(oversized);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, foundation::ErrorCode::too_large);

    auto excessiveCodePoints = observation();
    excessiveCodePoints.utf8Title = std::string(maximumWindowTitleCodePoints + 1U, 'a');
    const auto tooManyCodePoints = decodeWindowMetadata(excessiveCodePoints);
    ASSERT_FALSE(tooManyCodePoints);
    EXPECT_EQ(tooManyCodePoints.error().code, foundation::ErrorCode::too_large);
}

TEST(WindowMetadataTest, DerivesApplicationIdentityEvidenceFromStrictWmClass) {
    auto source = observation();
    source.wmClass = wmClass("prism-control", "PrismdrakeControl");
    const auto decoded = decodeWindowMetadata(source);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().identity.source, ApplicationIdentitySource::wmClass);
    ASSERT_TRUE(decoded.value().identity.instance);
    ASSERT_TRUE(decoded.value().identity.className);
    EXPECT_EQ(decoded.value().identity.instance.value(), "prism-control");
    EXPECT_EQ(decoded.value().identity.className.value(), "PrismdrakeControl");
    EXPECT_EQ(decoded.value().identity.groupingKey, "PrismdrakeControl");

    auto instanceOnly = observation();
    instanceOnly.wmClass = wmClass("instance-only", "");
    const auto fallback = decodeWindowMetadata(instanceOnly);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback.value().identity.groupingKey, "instance-only");
}

TEST(WindowMetadataTest, RejectsMalformedAndOversizedWmClassWithoutDisclosure) {
    auto malformed = observation();
    malformed.wmClass = std::vector<std::uint8_t>{'s', 'e', 'c', 'r', 'e', 't', 0U, 'x'};
    const auto missingTerminator = decodeWindowMetadata(malformed);
    ASSERT_FALSE(missingTerminator);
    EXPECT_EQ(missingTerminator.error().code, foundation::ErrorCode::validation_error);
    EXPECT_EQ(missingTerminator.error().message.find("secret"), std::string::npos);
    EXPECT_EQ(missingTerminator.error().recovery.find("secret"), std::string::npos);

    auto trailingBytes = observation();
    trailingBytes.wmClass = wmClass("instance", "Class");
    trailingBytes.wmClass->push_back('x');
    EXPECT_FALSE(decodeWindowMetadata(trailingBytes));

    auto oversized = observation();
    oversized.wmClass = std::vector<std::uint8_t>(maximumWmClassBytes + 1U, 'x');
    const auto tooLarge = decodeWindowMetadata(oversized);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, foundation::ErrorCode::too_large);

    auto oversizedPart = observation();
    oversizedPart.wmClass = wmClass(std::string(maximumWmClassPartBytes + 1U, 'x'), "Class");
    const auto partTooLarge = decodeWindowMetadata(oversizedPart);
    ASSERT_FALSE(partTooLarge);
    EXPECT_EQ(partTooLarge.error().code, foundation::ErrorCode::too_large);
}

TEST(WindowMetadataTest, DecodesTypeStateWorkspaceUrgencyAndTransientEvidence) {
    auto source = observation();
    source.windowTypes = {WindowType::dialog, WindowType::normal};
    source.windowStates = {WindowState::hidden, WindowState::fullscreen, WindowState::modal,
                           WindowState::skipTaskbar, WindowState::demandsAttention};
    source.workspace = allWorkspaces;
    source.wmHintsFlags = wmHintsUrgencyFlag;
    source.transientFor = window(7U);
    const auto decoded = decodeWindowMetadata(source);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().type, WindowType::dialog);
    EXPECT_TRUE(decoded.value().typeWasExplicit);
    EXPECT_EQ(decoded.value().states, source.windowStates);
    EXPECT_FALSE(decoded.value().workspace);
    EXPECT_TRUE(decoded.value().onAllWorkspaces);
    EXPECT_TRUE(decoded.value().minimized);
    EXPECT_TRUE(decoded.value().urgent);
    EXPECT_TRUE(decoded.value().skipTaskbar);
    EXPECT_TRUE(decoded.value().modal);
    EXPECT_EQ(decoded.value().transientFor, window(7U));

    auto numberedWorkspace = observation();
    numberedWorkspace.workspace = 3U;
    numberedWorkspace.wmHintsFlags = wmHintsUrgencyFlag;
    const auto numbered = decodeWindowMetadata(numberedWorkspace);
    ASSERT_TRUE(numbered);
    EXPECT_EQ(numbered.value().workspace, 3U);
    EXPECT_FALSE(numbered.value().onAllWorkspaces);
    EXPECT_TRUE(numbered.value().urgent);
}

TEST(WindowMetadataTest, RejectsInvalidDuplicateAndOversizedEnumsAndSelfTransient) {
    auto invalidType = observation();
    invalidType.windowTypes = {static_cast<WindowType>(99U)};
    EXPECT_FALSE(decodeWindowMetadata(invalidType));

    auto duplicateState = observation();
    duplicateState.windowStates = {WindowState::hidden, WindowState::hidden};
    EXPECT_FALSE(decodeWindowMetadata(duplicateState));

    auto oversizedStates = observation();
    oversizedStates.windowStates =
        std::vector<WindowState>(maximumWindowStates + 1U, WindowState::modal);
    const auto tooLarge = decodeWindowMetadata(oversizedStates);
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, foundation::ErrorCode::too_large);

    auto selfTransient = observation();
    selfTransient.transientFor = selfTransient.window;
    EXPECT_FALSE(decodeWindowMetadata(selfTransient));
}

TEST(WindowMetadataTest, DecodesMultipleStrictlyBoundedNetWmIcons) {
    auto source = observation();
    source.netWmIcon =
        std::vector<std::uint32_t>{2U, 1U, 0xff010203U, 0x7f040506U, 1U, 1U, 0xaabbccddU};
    const auto decoded = decodeWindowMetadata(source);

    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().icons.size(), 2U);
    EXPECT_EQ(decoded.value().icons[0], (WindowIcon{2U, 1U, {0xff010203U, 0x7f040506U}}));
    EXPECT_EQ(decoded.value().icons[1], (WindowIcon{1U, 1U, {0xaabbccddU}}));

    auto empty = observation();
    empty.netWmIcon = std::vector<std::uint32_t>{};
    const auto noIcons = decodeWindowMetadata(empty);
    ASSERT_TRUE(noIcons);
    EXPECT_TRUE(noIcons.value().icons.empty());
}

TEST(WindowMetadataTest, RejectsMalformedAndOversizedNetWmIcons) {
    auto truncated = observation();
    truncated.netWmIcon = std::vector<std::uint32_t>{2U, 2U, 1U};
    const auto malformed = decodeWindowMetadata(truncated);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, foundation::ErrorCode::validation_error);

    auto zeroDimension = observation();
    zeroDimension.netWmIcon = std::vector<std::uint32_t>{0U, 1U};
    EXPECT_FALSE(decodeWindowMetadata(zeroDimension));

    auto excessiveDimension = observation();
    excessiveDimension.netWmIcon = std::vector<std::uint32_t>{maximumWindowIconDimension + 1U, 1U};
    const auto dimensionTooLarge = decodeWindowMetadata(excessiveDimension);
    ASSERT_FALSE(dimensionTooLarge);
    EXPECT_EQ(dimensionTooLarge.error().code, foundation::ErrorCode::too_large);

    auto tooMany = observation();
    tooMany.netWmIcon = std::vector<std::uint32_t>{};
    for (std::size_t index = 0U; index <= maximumWindowIcons; ++index) {
        tooMany.netWmIcon->insert(tooMany.netWmIcon->end(), {1U, 1U, 0U});
    }
    const auto iconCountTooLarge = decodeWindowMetadata(tooMany);
    ASSERT_FALSE(iconCountTooLarge);
    EXPECT_EQ(iconCountTooLarge.error().code, foundation::ErrorCode::too_large);

    auto propertyTooLarge = observation();
    propertyTooLarge.netWmIcon =
        std::vector<std::uint32_t>(maximumNetWmIconBytes / sizeof(std::uint32_t) + 1U, 0xfeedbeefU);
    const auto bytesTooLarge = decodeWindowMetadata(propertyTooLarge);
    ASSERT_FALSE(bytesTooLarge);
    EXPECT_EQ(bytesTooLarge.error().code, foundation::ErrorCode::too_large);
    EXPECT_EQ(bytesTooLarge.error().message.find("feedbeef"), std::string::npos);
    EXPECT_EQ(bytesTooLarge.error().recovery.find("feedbeef"), std::string::npos);
}

} // namespace
} // namespace prismdrake::x11
