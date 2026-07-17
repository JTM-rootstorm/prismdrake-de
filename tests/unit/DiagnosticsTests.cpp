#include "Diagnostics.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace prismdrake::foundation {
namespace {

template <typename EventId>
concept AcceptedDiagnosticEventId = requires(EventId event_id) {
    makeDiagnosticEvent(DiagnosticComponent::foundation, DiagnosticSeverity::info, event_id);
};

static_assert(!AcceptedDiagnosticEventId<std::string>);
static_assert(!AcceptedDiagnosticEventId<std::string_view>);

TEST(DiagnosticsTest, RendersOnlyValidatedStructuredFields) {
    const auto generation = Generation::fromPublished(42);
    ASSERT_TRUE(generation);

    const auto event =
        makeDiagnosticEvent(DiagnosticComponent::settingsd, DiagnosticSeverity::warning,
                            DiagnosticEventId::snapshot_rejected, generation.value(),
                            DiagnosticProfile::lustre, DiagnosticRecovery::review_configuration);

    EXPECT_EQ(renderDiagnosticEvent(event), "component=prismdrake-settingsd severity=warning "
                                            "event=snapshot_rejected generation=42 profile=lustre "
                                            "recovery=review_configuration");
}

TEST(DiagnosticsTest, StableEventIdsAndRenderedEventsRemainBounded) {
    constexpr std::array event_ids{
        DiagnosticEventId::invalid_configuration,  DiagnosticEventId::unsupported_schema_version,
        DiagnosticEventId::component_start_failed, DiagnosticEventId::component_restart_exhausted,
        DiagnosticEventId::dependency_unavailable, DiagnosticEventId::capability_unavailable,
        DiagnosticEventId::snapshot_rejected,      DiagnosticEventId::snapshot_published,
        DiagnosticEventId::fallback_selected,      DiagnosticEventId::operation_cancelled,
        DiagnosticEventId::internal_error,
    };
    for (const auto event_id : event_ids) {
        EXPECT_LE(diagnosticEventId(event_id).size(), maximumDiagnosticEventIdLength);
    }

    const auto generation =
        Generation::fromPublished(std::numeric_limits<Generation::Value>::max());
    ASSERT_TRUE(generation);
    const auto event =
        makeDiagnosticEvent(DiagnosticComponent::polkit_agent, DiagnosticSeverity::critical,
                            DiagnosticEventId::component_restart_exhausted, generation.value(),
                            DiagnosticProfile::lustre, DiagnosticRecovery::contact_administrator);
    EXPECT_LE(renderDiagnosticEvent(event).size(), maximumRenderedDiagnosticLength);
}

TEST(DiagnosticsTest, HasNoFreeFormPrivateContentInput) {
    const auto event = makeDiagnosticEvent(DiagnosticComponent::shell, DiagnosticSeverity::error,
                                           DiagnosticEventId::internal_error);
    const std::string rendered = renderDiagnosticEvent(event);
    EXPECT_EQ(rendered.find("/home/mike/private"), std::string::npos);
    EXPECT_EQ(rendered.find("token=secret"), std::string::npos);
}

} // namespace
} // namespace prismdrake::foundation
