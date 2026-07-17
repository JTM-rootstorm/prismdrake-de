#include "Diagnostics.hpp"

namespace prismdrake::foundation {
DiagnosticEvent makeDiagnosticEvent(DiagnosticComponent component, DiagnosticSeverity severity,
                                    DiagnosticEventId event_id,
                                    std::optional<Generation> generation,
                                    std::optional<DiagnosticProfile> profile,
                                    DiagnosticRecovery recovery) {
    return DiagnosticEvent(component, severity, event_id, generation, profile, recovery);
}

std::string renderDiagnosticEvent(const DiagnosticEvent &event) {
    std::string rendered;
    rendered.reserve(256);
    rendered.append("component=").append(diagnosticComponentId(event.component()));
    rendered.append(" severity=").append(diagnosticSeverityId(event.severity()));
    rendered.append(" event=").append(diagnosticEventId(event.eventId()));
    rendered.append(" generation=");
    if (event.generation()) {
        rendered.append(std::to_string(event.generation()->value()));
    } else {
        rendered.append("none");
    }
    rendered.append(" profile=");
    if (event.profile()) {
        rendered.append(diagnosticProfileId(*event.profile()));
    } else {
        rendered.append("none");
    }
    rendered.append(" recovery=").append(diagnosticRecoveryId(event.recovery()));
    if (rendered.size() > maximumRenderedDiagnosticLength) {
        return "component=prismdrake-foundation severity=error event=internal_error "
               "generation=none profile=none recovery=none";
    }
    return rendered;
}

std::string_view diagnosticComponentId(DiagnosticComponent component) noexcept {
    switch (component) {
    case DiagnosticComponent::foundation:
        return "prismdrake-foundation";
    case DiagnosticComponent::session:
        return "prismdrake-session";
    case DiagnosticComponent::settingsd:
        return "prismdrake-settingsd";
    case DiagnosticComponent::notifyd:
        return "prismdrake-notifyd";
    case DiagnosticComponent::shell:
        return "prismdrake-shell";
    case DiagnosticComponent::decor:
        return "prismdrake-decor";
    case DiagnosticComponent::control:
        return "prismdrake-control";
    case DiagnosticComponent::portal:
        return "prismdrake-portal";
    case DiagnosticComponent::polkit_agent:
        return "prismdrake-polkit-agent";
    case DiagnosticComponent::lock:
        return "prismdrake-lock";
    case DiagnosticComponent::themes:
        return "prismdrake-themes";
    case DiagnosticComponent::style_qt:
        return "prismdrake-style-qt";
    case DiagnosticComponent::theme_gtk:
        return "prismdrake-theme-gtk";
    }
    return "prismdrake-foundation";
}

std::string_view diagnosticSeverityId(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::debug:
        return "debug";
    case DiagnosticSeverity::info:
        return "info";
    case DiagnosticSeverity::warning:
        return "warning";
    case DiagnosticSeverity::error:
        return "error";
    case DiagnosticSeverity::critical:
        return "critical";
    }
    return "error";
}

std::string_view diagnosticEventId(DiagnosticEventId event_id) noexcept {
    switch (event_id) {
    case DiagnosticEventId::invalid_configuration:
        return "invalid_configuration";
    case DiagnosticEventId::unsupported_schema_version:
        return "unsupported_schema_version";
    case DiagnosticEventId::component_start_failed:
        return "component_start_failed";
    case DiagnosticEventId::component_restart_exhausted:
        return "component_restart_exhausted";
    case DiagnosticEventId::dependency_unavailable:
        return "dependency_unavailable";
    case DiagnosticEventId::capability_unavailable:
        return "capability_unavailable";
    case DiagnosticEventId::snapshot_rejected:
        return "snapshot_rejected";
    case DiagnosticEventId::snapshot_published:
        return "snapshot_published";
    case DiagnosticEventId::fallback_selected:
        return "fallback_selected";
    case DiagnosticEventId::operation_cancelled:
        return "operation_cancelled";
    case DiagnosticEventId::internal_error:
        return "internal_error";
    }
    return "internal_error";
}

std::string_view diagnosticProfileId(DiagnosticProfile profile) noexcept {
    switch (profile) {
    case DiagnosticProfile::lustre:
        return "lustre";
    case DiagnosticProfile::forge:
        return "forge";
    }
    return "forge";
}

std::string_view diagnosticRecoveryId(DiagnosticRecovery recovery) noexcept {
    switch (recovery) {
    case DiagnosticRecovery::none:
        return "none";
    case DiagnosticRecovery::retry:
        return "retry";
    case DiagnosticRecovery::restart_component:
        return "restart_component";
    case DiagnosticRecovery::use_fallback:
        return "use_fallback";
    case DiagnosticRecovery::review_configuration:
        return "review_configuration";
    case DiagnosticRecovery::contact_administrator:
        return "contact_administrator";
    }
    return "none";
}

} // namespace prismdrake::foundation
