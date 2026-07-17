#include "RandrTopology.hpp"

#include <gtest/gtest.h>

#include <array>

namespace prismdrake::x11 {
namespace {

constexpr std::uint8_t firstEvent = 80U;
constexpr std::uint8_t screenChange = firstEvent;
constexpr std::uint8_t notify = firstEvent + 1U;

TEST(RandrTopologyTest, SelectsOnlyTopologyRefreshMasks) {
    EXPECT_EQ(randr12TopologyEventMask,
              randrScreenChangeMask | randrCrtcChangeMask | randrOutputChangeMask);
    EXPECT_EQ(randrTopologyEventMask, randrScreenChangeMask | randrCrtcChangeMask |
                                          randrOutputChangeMask | randrResourceChangeMask);
    EXPECT_EQ(randrTopologyEventMask & 8U, 0U);
    EXPECT_EQ(randrTopologyEventMask & 16U, 0U);
    EXPECT_EQ(randrTopologyEventMask & 32U, 0U);
    EXPECT_EQ(randrTopologyEventMask & 128U, 0U);
}

TEST(RandrTopologyTest, RelevantEventsRequireCompleteRequery) {
    EXPECT_TRUE(randrEventRequiresFullRequery(firstEvent, {screenChange, 0U}, false));
    EXPECT_TRUE(randrEventRequiresFullRequery(firstEvent, {notify, 0U}, false));
    EXPECT_TRUE(randrEventRequiresFullRequery(firstEvent, {notify, 1U}, false));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 5U}, false));
    EXPECT_TRUE(randrEventRequiresFullRequery(firstEvent, {notify, 5U}, true));

    EXPECT_TRUE(randrEventRequiresFullRequery(
        firstEvent, {static_cast<std::uint8_t>(screenChange | 0x80U), 0U}, false));
}

TEST(RandrTopologyTest, IgnoresPropertyProviderLeaseAndUnrelatedTraffic) {
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 2U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 3U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 4U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 6U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {notify, 255U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(firstEvent, {70U, 0U}, true));
    EXPECT_FALSE(randrEventRequiresFullRequery(0U, {screenChange, 0U}, true));
}

TEST(RandrTopologyTest, StatusIdentifiersAreFixedAndRedacted) {
    EXPECT_EQ(randrTopologyStatusId(RandrTopologyStatus::unavailable), "randr_unavailable");
    EXPECT_EQ(randrTopologyStatusId(RandrTopologyStatus::malformed), "randr_malformed");
    EXPECT_EQ(randrTopologyStatusId(RandrTopologyStatus::randr_1_2), "randr_1_2");
    EXPECT_EQ(randrTopologyStatusId(RandrTopologyStatus::randr_1_3), "randr_1_3");
    EXPECT_EQ(randrTopologyStatusId(RandrTopologyStatus::randr_1_4), "randr_1_4");
}

TEST(RandrTopologyTest, AcceptsOnlyConsistentOutputInfoIdentifierLists) {
    const std::array<std::uint32_t, 2> resourceCrtcs{10U, 11U};
    const std::array<std::uint32_t, 3> resourceOutputs{20U, 21U, 22U};
    const std::array<std::uint32_t, 2> resourceModes{30U, 31U};
    std::array<std::uint32_t, 2> allowedCrtcs{11U, 10U};
    std::array<std::uint32_t, 2> modes{31U, 30U};
    std::array<std::uint32_t, 1> clones{21U};
    const RandrResourceIdsView resources{resourceCrtcs, resourceOutputs, resourceModes};

    EXPECT_TRUE(randrOutputInfoListsAreValid({10U, allowedCrtcs, modes, clones}, resources));
    EXPECT_TRUE(randrOutputInfoListsAreValid({0U, allowedCrtcs, modes, clones}, resources));

    std::array<std::uint32_t, 2> duplicateCrtcs{10U, 10U};
    std::array<std::uint32_t, 2> zeroMode{30U, 0U};
    std::array<std::uint32_t, 1> unknownClone{99U};
    std::array<std::uint32_t, 2> duplicateModes{30U, 30U};
    std::array<std::uint32_t, 2> duplicateClones{21U, 21U};
    std::array<std::uint32_t, 1> unknownCrtc{99U};
    EXPECT_FALSE(randrOutputInfoListsAreValid({10U, duplicateCrtcs, modes, clones}, resources));
    EXPECT_FALSE(randrOutputInfoListsAreValid({10U, allowedCrtcs, zeroMode, clones}, resources));
    EXPECT_FALSE(randrOutputInfoListsAreValid({10U, allowedCrtcs, modes, unknownClone}, resources));
    EXPECT_FALSE(
        randrOutputInfoListsAreValid({10U, allowedCrtcs, duplicateModes, clones}, resources));
    EXPECT_FALSE(
        randrOutputInfoListsAreValid({10U, allowedCrtcs, modes, duplicateClones}, resources));
    EXPECT_FALSE(randrOutputInfoListsAreValid({10U, unknownCrtc, modes, clones}, resources));
    EXPECT_FALSE(randrOutputInfoListsAreValid({12U, allowedCrtcs, modes, clones}, resources));
}

TEST(RandrTopologyTest, AcceptsOnlyConsistentCrtcOutputLists) {
    const std::array<std::uint32_t, 3> resources{20U, 21U, 22U};
    std::array<std::uint32_t, 2> current{21U, 20U};
    std::array<std::uint32_t, 3> possible{22U, 20U, 21U};

    EXPECT_TRUE(randrCrtcInfoListsAreValid({20U, current, possible}, resources));

    std::array<std::uint32_t, 2> duplicateCurrent{20U, 20U};
    std::array<std::uint32_t, 1> unknownPossible{99U};
    std::array<std::uint32_t, 1> incompletePossible{20U};
    std::array<std::uint32_t, 2> duplicatePossible{20U, 20U};
    std::array<std::uint32_t, 2> zeroCurrent{20U, 0U};
    EXPECT_FALSE(randrCrtcInfoListsAreValid({20U, duplicateCurrent, possible}, resources));
    EXPECT_FALSE(randrCrtcInfoListsAreValid({20U, current, unknownPossible}, resources));
    EXPECT_FALSE(randrCrtcInfoListsAreValid({20U, current, incompletePossible}, resources));
    EXPECT_FALSE(randrCrtcInfoListsAreValid({20U, current, duplicatePossible}, resources));
    EXPECT_FALSE(randrCrtcInfoListsAreValid({20U, zeroCurrent, possible}, resources));
    EXPECT_FALSE(randrCrtcInfoListsAreValid({22U, current, possible}, resources));
}

TEST(RandrTopologyTest, RequiresResourceModeNamesToExactlyFillNamesPayload) {
    const std::array<std::uint16_t, 3> lengths{4U, 0U, 7U};
    EXPECT_TRUE(randrModeNameLengthsMatch(lengths, 11U));
    EXPECT_FALSE(randrModeNameLengthsMatch(lengths, 10U));
    EXPECT_FALSE(randrModeNameLengthsMatch(lengths, 12U));
}

} // namespace
} // namespace prismdrake::x11
