#include "RootEventStream.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] WindowId rootWindow() { return WindowId::fromProtocol(100U).value(); }

[[nodiscard]] AtomId atomId(AtomId::Value value) { return AtomId::fromProtocol(value).value(); }

template <typename T> [[nodiscard]] const T &requireEvent(const RootEvent &event) {
    const auto *value = std::get_if<T>(&event);
    EXPECT_NE(value, nullptr);
    if (value == nullptr) {
        throw std::logic_error("decoded root event has an unexpected type");
    }
    return *value;
}

template <typename T>
concept ExposesWindowIdentifier = requires(const T &hint) { hint.window; };

static_assert(!ExposesWindowIdentifier<ClientTopologyHint>);
static_assert(!ExposesWindowIdentifier<OutputTopologyRefreshHint>);

TEST(RootEventDecoderTest, UsesOnlyNotifyMasksAndNeverSelectsRedirectAuthority) {
    EXPECT_EQ(rootEventSelectionMask,
              structureNotifyEventMask | substructureNotifyEventMask | propertyChangeEventMask);
    EXPECT_EQ(rootEventSelectionMask & substructureRedirectEventMask, 0U);
    EXPECT_EQ(maximumRootEventsPerDrain, 256U);
}

TEST(RootEventDecoderTest, KeepsOutputTopologyRefreshDistinctAndRedacted) {
    const RootEvent actual = OutputTopologyRefreshHint{false};
    EXPECT_EQ(requireEvent<OutputTopologyRefreshHint>(actual), OutputTopologyRefreshHint{false});
    EXPECT_EQ(std::get_if<RootGeometryHint>(&actual), nullptr);

    const RootEvent syntheticHint = OutputTopologyRefreshHint{true};
    EXPECT_TRUE(requireEvent<OutputTopologyRefreshHint>(syntheticHint).synthetic);
}

TEST(RootEventDecoderTest, DecodesActualAndSyntheticCreateAsRefreshHints) {
    const CreateNotifyFields actual{createNotifyEventType, 100U, 200U, 1, 2, 640U, 480U, 0U, false};
    auto decoded = decodeRootEvent(actual, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    const auto &actualHint = requireEvent<ClientTopologyHint>(*decoded.value());
    EXPECT_EQ(actualHint.change, ClientTopologyChange::created);
    EXPECT_FALSE(actualHint.synthetic);

    auto syntheticFields = actual;
    syntheticFields.responseType |= syntheticEventBit;
    decoded = decodeRootEvent(syntheticFields, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_TRUE(requireEvent<ClientTopologyHint>(*decoded.value()).synthetic);
}

TEST(RootEventDecoderTest, DecodesActualAndSyntheticDestroyAsRefreshHints) {
    const DestroyNotifyFields actual{destroyNotifyEventType, 100U, 200U};
    auto decoded = decodeRootEvent(actual, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    const auto &actualHint = requireEvent<ClientTopologyHint>(*decoded.value());
    EXPECT_EQ(actualHint.change, ClientTopologyChange::destroyed);
    EXPECT_FALSE(actualHint.synthetic);

    auto syntheticFields = actual;
    syntheticFields.responseType |= syntheticEventBit;
    decoded = decodeRootEvent(syntheticFields, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_TRUE(requireEvent<ClientTopologyHint>(*decoded.value()).synthetic);
}

TEST(RootEventDecoderTest, IgnoresUnrelatedChildrenAndRejectsMalformedRelevantLifecycle) {
    const CreateNotifyFields unrelated{createNotifyEventType, 101U, 200U, 0, 0, 1U, 1U, 0U, false};
    const auto ignored = decodeRootEvent(unrelated, rootWindow());
    ASSERT_TRUE(ignored);
    EXPECT_FALSE(ignored.value());

    const CreateNotifyFields zeroWindow{createNotifyEventType, 100U, 0U, 0, 0, 1U, 1U, 0U, false};
    EXPECT_FALSE(decodeRootEvent(zeroWindow, rootWindow()));

    const CreateNotifyFields zeroGeometry{
        createNotifyEventType, 100U, 200U, 0, 0, 0U, 1U, 0U, false};
    EXPECT_FALSE(decodeRootEvent(zeroGeometry, rootWindow()));

    const DestroyNotifyFields destroysRoot{destroyNotifyEventType, 100U, 100U};
    EXPECT_FALSE(decodeRootEvent(destroysRoot, rootWindow()));
}

TEST(RootEventDecoderTest, DecodesRootPropertyStateAndPreservesSyntheticFlag) {
    const PropertyNotifyFields changed{propertyNotifyEventType, 100U, atomId(55U), 0U};
    auto decoded = decodeRootEvent(changed, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_EQ(requireEvent<RootPropertyHint>(*decoded.value()),
              (RootPropertyHint{atomId(55U), RootPropertyState::newValue, false}));

    const PropertyNotifyFields deleted{
        static_cast<std::uint8_t>(propertyNotifyEventType | syntheticEventBit), 100U, atomId(55U),
        1U};
    decoded = decodeRootEvent(deleted, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_EQ(requireEvent<RootPropertyHint>(*decoded.value()),
              (RootPropertyHint{atomId(55U), RootPropertyState::deleted, true}));
}

TEST(RootEventDecoderTest, IgnoresNonRootPropertiesAndRejectsMalformedRelevantProperties) {
    const PropertyNotifyFields unrelated{propertyNotifyEventType, 200U, atomId(55U), 0U};
    const auto ignored = decodeRootEvent(unrelated, rootWindow());
    ASSERT_TRUE(ignored);
    EXPECT_FALSE(ignored.value());

    const PropertyNotifyFields noAtom{propertyNotifyEventType, 100U, std::nullopt, 0U};
    EXPECT_FALSE(decodeRootEvent(noAtom, rootWindow()));
    const PropertyNotifyFields badState{propertyNotifyEventType, 100U, atomId(55U), 2U};
    EXPECT_FALSE(decodeRootEvent(badState, rootWindow()));
}

TEST(RootEventDecoderTest, DecodesOnlyValidRootConfigureAsGeometryRefreshHint) {
    const ConfigureNotifyFields configured{
        configureNotifyEventType, 100U, 100U, 0, 0, 1920U, 1080U, 0U};
    auto decoded = decodeRootEvent(configured, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_EQ(requireEvent<RootGeometryHint>(*decoded.value()), RootGeometryHint{false});

    auto syntheticFields = configured;
    syntheticFields.responseType |= syntheticEventBit;
    decoded = decodeRootEvent(syntheticFields, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_EQ(requireEvent<RootGeometryHint>(*decoded.value()), RootGeometryHint{true});

    auto childConfigured = configured;
    childConfigured.window = 200U;
    const auto ignored = decodeRootEvent(childConfigured, rootWindow());
    ASSERT_TRUE(ignored);
    EXPECT_FALSE(ignored.value());

    auto zeroGeometry = configured;
    zeroGeometry.width = 0U;
    EXPECT_FALSE(decodeRootEvent(zeroGeometry, rootWindow()));
}

TEST(RootEventDecoderTest, RejectsMismatchedProtocolTypesWithoutReflectingFields) {
    constexpr WindowId::Value sentinelWindow = 0xdeadbeefU;
    const CreateNotifyFields mismatched{
        destroyNotifyEventType, 100U, sentinelWindow, 0, 0, 1U, 1U, 0U, false};
    const auto decoded = decodeRootEvent(mismatched, rootWindow());
    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().message.find("deadbeef"), std::string::npos);
    EXPECT_EQ(decoded.error().recovery.find("deadbeef"), std::string::npos);
}

TEST(RootEventDecoderTest, DecodesProtocolErrorsAsRedactedRecoverableRefreshHints) {
    const ProtocolErrorFields protocolError{0U, 3U, 20U, 0U};
    auto decoded = decodeRootEvent(protocolError, rootWindow());
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded.value());
    EXPECT_EQ(requireEvent<ProtocolErrorHint>(*decoded.value()), ProtocolErrorHint{});

    const ProtocolErrorFields missingErrorCode{0U, 0U, 20U, 0U};
    EXPECT_FALSE(decodeRootEvent(missingErrorCode, rootWindow()));

    const ProtocolErrorFields ordinaryEvent{createNotifyEventType, 3U, 20U, 0U};
    EXPECT_FALSE(decodeRootEvent(ordinaryEvent, rootWindow()));
}

} // namespace
} // namespace prismdrake::x11
