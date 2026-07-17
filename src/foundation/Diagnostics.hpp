#pragma once

#include "Generation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace prismdrake::foundation {

inline constexpr std::size_t maximumDiagnosticEventIdLength = 64;
inline constexpr std::size_t maximumRenderedDiagnosticLength = 256;

enum class DiagnosticComponent : std::uint8_t {
    foundation,
    session,
    settingsd,
    notifyd,
    shell,
    decor,
    control,
    portal,
    polkit_agent,
    lock,
    themes,
    style_qt,
    theme_gtk,
};

enum class DiagnosticSeverity : std::uint8_t {
    debug,
    info,
    warning,
    error,
    critical,
};

/// Stable machine identifiers; adding an event requires a reviewed code change.
enum class DiagnosticEventId : std::uint8_t {
    invalid_configuration,
    unsupported_schema_version,
    component_start_failed,
    component_restart_exhausted,
    dependency_unavailable,
    capability_unavailable,
    snapshot_rejected,
    snapshot_published,
    fallback_selected,
    operation_cancelled,
    internal_error,
};

enum class DiagnosticProfile : std::uint8_t {
    lustre,
    forge,
};

enum class DiagnosticRecovery : std::uint8_t {
    none,
    retry,
    restart_component,
    use_fallback,
    review_configuration,
    contact_administrator,
};

/// Bounded diagnostic metadata without free-form content, paths, or secret fields.
class DiagnosticEvent final {
  public:
    [[nodiscard]] DiagnosticComponent component() const noexcept { return component_; }
    [[nodiscard]] DiagnosticSeverity severity() const noexcept { return severity_; }
    [[nodiscard]] DiagnosticEventId eventId() const noexcept { return event_id_; }
    [[nodiscard]] const std::optional<Generation> &generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] std::optional<DiagnosticProfile> profile() const noexcept { return profile_; }
    [[nodiscard]] DiagnosticRecovery recovery() const noexcept { return recovery_; }

  private:
    friend DiagnosticEvent makeDiagnosticEvent(DiagnosticComponent, DiagnosticSeverity,
                                               DiagnosticEventId, std::optional<Generation>,
                                               std::optional<DiagnosticProfile>,
                                               DiagnosticRecovery);

    DiagnosticEvent(DiagnosticComponent component, DiagnosticSeverity severity,
                    DiagnosticEventId event_id, std::optional<Generation> generation,
                    std::optional<DiagnosticProfile> profile, DiagnosticRecovery recovery) noexcept
        : component_(component), severity_(severity), event_id_(event_id), generation_(generation),
          profile_(profile), recovery_(recovery) {}

    DiagnosticComponent component_;
    DiagnosticSeverity severity_;
    DiagnosticEventId event_id_;
    std::optional<Generation> generation_;
    std::optional<DiagnosticProfile> profile_;
    DiagnosticRecovery recovery_;
};

[[nodiscard]] DiagnosticEvent
makeDiagnosticEvent(DiagnosticComponent component, DiagnosticSeverity severity,
                    DiagnosticEventId event_id, std::optional<Generation> generation = std::nullopt,
                    std::optional<DiagnosticProfile> profile = std::nullopt,
                    DiagnosticRecovery recovery = DiagnosticRecovery::none);

/// Renders only the validated structured fields owned by DiagnosticEvent.
[[nodiscard]] std::string renderDiagnosticEvent(const DiagnosticEvent &event);

[[nodiscard]] std::string_view diagnosticComponentId(DiagnosticComponent component) noexcept;
[[nodiscard]] std::string_view diagnosticSeverityId(DiagnosticSeverity severity) noexcept;
[[nodiscard]] std::string_view diagnosticEventId(DiagnosticEventId event_id) noexcept;
[[nodiscard]] std::string_view diagnosticProfileId(DiagnosticProfile profile) noexcept;
[[nodiscard]] std::string_view diagnosticRecoveryId(DiagnosticRecovery recovery) noexcept;

} // namespace prismdrake::foundation
