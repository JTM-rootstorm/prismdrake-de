#include "ApplicationIdentity.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

[[nodiscard]] DesktopFileId desktopId(std::string value) {
    auto result = deriveDesktopFileId(value);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

[[nodiscard]] ObservedWindowIdentity window(std::uint32_t xid, std::uint64_t incarnation) {
    auto result = ObservedWindowIdentity::fromObservation(xid, incarnation);
    EXPECT_TRUE(result);
    return result.value();
}

[[nodiscard]] CatalogApplicationIdentity application(std::string id,
                                                     std::optional<std::string> startup = {}) {
    return {desktopId(std::move(id)), std::move(startup)};
}

TEST(ApplicationIdentityTest, UsesAcceptedSixStepOrderAndAsciiFolding) {
    const std::vector catalog{
        application("activation.desktop", "Other"), application("class.desktop", "CLASS-TOKEN"),
        application("instance.desktop", "instance-token"), application("identifier.desktop")};
    const std::vector windows{
        WindowAssociationObservation{window(1U, 1U), desktopId("activation.desktop"),
                                     "instance-token", "class-token"},
        WindowAssociationObservation{window(2U, 2U), std::nullopt, "instance-token", "class-token"},
        WindowAssociationObservation{window(3U, 3U), std::nullopt, "INSTANCE-TOKEN", "unmatched"},
        WindowAssociationObservation{window(4U, 4U), std::nullopt, "unmatched", "IDENTIFIER"},
        WindowAssociationObservation{window(5U, 5U), std::nullopt, "fallback", "FALLBACK"},
        WindowAssociationObservation{window(6U, 6U), std::nullopt, std::nullopt, std::nullopt}};

    const auto result = associateApplications(catalog, windows, 7U);

    ASSERT_TRUE(result);
    const auto &records = result.value()->associations;
    ASSERT_EQ(records.size(), 6U);
    EXPECT_EQ(records[0].applicationId.value(), "activation.desktop");
    EXPECT_EQ(records[0].evidence, ApplicationAssociationEvidence::activationDesktopFileId);
    EXPECT_EQ(records[1].applicationId.value(), "class.desktop");
    EXPECT_EQ(records[1].evidence, ApplicationAssociationEvidence::startupWmClassClass);
    EXPECT_EQ(records[2].applicationId.value(), "instance.desktop");
    EXPECT_EQ(records[2].evidence, ApplicationAssociationEvidence::startupWmClassInstance);
    EXPECT_EQ(records[3].applicationId.value(), "identifier.desktop");
    EXPECT_EQ(records[3].evidence, ApplicationAssociationEvidence::desktopFileIdMatch);
    EXPECT_EQ(records[4].applicationId.value(), "wm-class:fallback");
    EXPECT_EQ(records[4].evidence, ApplicationAssociationEvidence::wmClassFallback);
    EXPECT_EQ(records[5].applicationId.value(), "unknown-application");
    EXPECT_EQ(records[5].evidence, ApplicationAssociationEvidence::unknownApplication);
}

TEST(ApplicationIdentityTest, AmbiguityPublishesFallbackAndOnlyRedactedCategory) {
    const std::vector catalog{application("first.desktop", "Same"),
                              application("second.desktop", "same")};
    const std::vector windows{
        WindowAssociationObservation{window(8U, 9U), std::nullopt, "unused", "SAME"}};

    const auto result = associateApplications(catalog, windows, 1U);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value()->associations.size(), 1U);
    EXPECT_EQ(result.value()->associations.front().applicationId.value(), "wm-class:same");
    ASSERT_EQ(result.value()->diagnostics.size(), 1U);
    EXPECT_EQ(result.value()->diagnostics.front().code,
              ApplicationAssociationDiagnosticCode::ambiguousStartupWmClassClass);
    EXPECT_EQ(result.value()->diagnostics.front().window, window(8U, 9U));
}

TEST(ApplicationIdentityTest, PreservesNonAsciiAndIncarnationInImmutableSnapshot) {
    const std::vector catalog{application("unicode.desktop", "Äpp")};
    const std::vector windows{
        WindowAssociationObservation{window(10U, 100U), std::nullopt, std::nullopt, "Äpp"},
        WindowAssociationObservation{window(10U, 101U), std::nullopt, std::nullopt, "äpp"}};

    const auto result = associateApplications(catalog, windows, 12U);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value()->associations[0].applicationId.value(), "unicode.desktop");
    EXPECT_EQ(result.value()->associations[1].applicationId.value(), "wm-class:äpp");
    EXPECT_NE(result.value()->associations[0].window, result.value()->associations[1].window);
}

TEST(ApplicationIdentityTest, RejectsDuplicateAndMalformedEvidenceWithoutPublishing) {
    const std::vector duplicateCatalog{application("same.desktop"), application("same.desktop")};
    const std::vector validWindows{
        WindowAssociationObservation{window(1U, 1U), std::nullopt, std::nullopt, std::nullopt}};
    EXPECT_FALSE(associateApplications(duplicateCatalog, validWindows, 1U));

    const std::vector catalog{application("safe.desktop")};
    const std::vector duplicateWindows{
        WindowAssociationObservation{window(1U, 1U), std::nullopt, std::nullopt, "safe"},
        WindowAssociationObservation{window(1U, 1U), std::nullopt, std::nullopt, "safe"}};
    EXPECT_FALSE(associateApplications(catalog, duplicateWindows, 1U));
    EXPECT_FALSE(associateApplications(catalog, validWindows, 0U));
    EXPECT_FALSE(ObservedWindowIdentity::fromObservation(0U, 1U));
    EXPECT_FALSE(ObservedWindowIdentity::fromObservation(1U, 0U));
}

} // namespace
} // namespace prismdrake::launcher
