#pragma once

#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::shell::composition {

inline constexpr std::size_t maximumCompositionOutputs = 64U;

class OutputName final {
  public:
    [[nodiscard]] static foundation::Result<OutputName> create(std::string value);
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    friend bool operator==(const OutputName &, const OutputName &) = default;

  private:
    explicit OutputName(std::string value) noexcept : value_(std::move(value)) {}

    std::string value_;
};

enum class FocusControlRole : std::uint8_t {
    panelLauncher,
    panelNotificationCenter,
    panelQuickSettings,
    panelTaskGroup,
    launcherResult,
    notificationCard,
    logoutControl,
};

class FocusControlId final {
  public:
    [[nodiscard]] static foundation::Result<FocusControlId> create(std::string value);
    [[nodiscard]] std::string_view value() const noexcept { return value_; }

    friend bool operator==(const FocusControlId &, const FocusControlId &) = default;

  private:
    explicit FocusControlId(std::string value) noexcept : value_(std::move(value)) {}

    std::string value_;
};

enum class FocusOriginKind : std::uint8_t {
    control,
    windowManager,
};

/// An opaque shell-owned presentation identity. It never carries an XID,
/// desktop-entry path, D-Bus payload, or JavaScript numeric identity.
class FocusOrigin final {
  public:
    [[nodiscard]] static foundation::Result<FocusOrigin>
    control(OutputName output, FocusControlRole role, FocusControlId controlId);
    [[nodiscard]] static FocusOrigin windowManager() noexcept;

    [[nodiscard]] FocusOriginKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::optional<OutputName> &output() const noexcept { return output_; }
    [[nodiscard]] const std::optional<FocusControlRole> &role() const noexcept { return role_; }
    [[nodiscard]] const std::optional<FocusControlId> &controlId() const noexcept {
        return control_id_;
    }
    [[nodiscard]] bool equivalent() const noexcept { return equivalent_; }

    friend bool operator==(const FocusOrigin &, const FocusOrigin &) = default;

  private:
    FocusOrigin(FocusOriginKind kind, std::optional<OutputName> output,
                std::optional<FocusControlRole> role, std::optional<FocusControlId> controlId,
                bool equivalent) noexcept;

    [[nodiscard]] FocusOrigin transferredTo(OutputName output) const;

    friend class ShellComposition;

    FocusOriginKind kind_;
    std::optional<OutputName> output_;
    std::optional<FocusControlRole> role_;
    std::optional<FocusControlId> control_id_;
    bool equivalent_;
};

struct OutputPlacement final {
    OutputName name;
    bool hasPanel;

    friend bool operator==(const OutputPlacement &, const OutputPlacement &) = default;
};

class OutputPlacementSnapshot final {
  public:
    [[nodiscard]] static foundation::Result<OutputPlacementSnapshot>
    create(std::uint64_t generation, std::vector<OutputPlacement> outputs,
           std::optional<OutputName> primaryOutput);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::span<const OutputPlacement> outputs() const noexcept { return outputs_; }
    [[nodiscard]] const std::optional<OutputName> &primaryOutput() const noexcept {
        return primary_output_;
    }

  private:
    OutputPlacementSnapshot(std::uint64_t generation, std::vector<OutputPlacement> outputs,
                            std::optional<OutputName> primaryOutput) noexcept;

    std::uint64_t generation_;
    std::vector<OutputPlacement> outputs_;
    std::optional<OutputName> primary_output_;
};

class OwnerEpoch final {
  public:
    [[nodiscard]] static foundation::Result<OwnerEpoch> create(std::uint64_t value);
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

    friend bool operator==(const OwnerEpoch &, const OwnerEpoch &) = default;

  private:
    explicit OwnerEpoch(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_;
};

class OwnerGeneration final {
  public:
    [[nodiscard]] static foundation::Result<OwnerGeneration> create(std::uint64_t value);
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

    friend bool operator==(const OwnerGeneration &, const OwnerGeneration &) = default;

  private:
    explicit OwnerGeneration(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_;
};

struct AuthoritySnapshot final {
    OwnerEpoch epoch;
    OwnerGeneration generation;

    friend bool operator==(const AuthoritySnapshot &, const AuthoritySnapshot &) = default;
};

enum class SurfaceFamily : std::uint8_t {
    logoutConfirmation,
    actionMenu,
    quickSettings,
    notificationCenter,
    launcher,
    transientNotificationAction,
};

enum class DismissReason : std::uint8_t {
    explicitRequest,
    escape,
    conflict,
    outputRemoved,
    settingsOwnerChanged,
    notificationOwnerChanged,
};

struct PresentSurfaceIntent final {
    SurfaceFamily family;
    OutputName output;
};

struct MoveSurfaceIntent final {
    SurfaceFamily family;
    OutputName from;
    OutputName to;
};

struct DismissSurfaceIntent final {
    SurfaceFamily family;
    OutputName output;
    DismissReason reason;
};

struct FocusSurfaceIntent final {
    SurfaceFamily family;
    OutputName output;
};

struct RestoreControlFocusIntent final {
    FocusOrigin origin;
};

struct ReleaseFocusToWindowManagerIntent final {};
struct CancelLogoutIntent final {};

enum class OptionalDependency : std::uint8_t {
    notificationAuthority,
};

struct DependencyAvailabilityIntent final {
    SurfaceFamily family;
    OptionalDependency dependency;
    bool available;
};

using CompositionIntent =
    std::variant<PresentSurfaceIntent, MoveSurfaceIntent, DismissSurfaceIntent, FocusSurfaceIntent,
                 RestoreControlFocusIntent, ReleaseFocusToWindowManagerIntent, CancelLogoutIntent,
                 DependencyAvailabilityIntent>;

enum class TransitionDisposition : std::uint8_t {
    applied,
    blockedByHigherPriority,
};

class CompositionTransition final {
  public:
    [[nodiscard]] TransitionDisposition disposition() const noexcept { return disposition_; }
    [[nodiscard]] std::span<const CompositionIntent> intents() const noexcept { return intents_; }

  private:
    friend class ShellComposition;
    TransitionDisposition disposition_{TransitionDisposition::applied};
    std::vector<CompositionIntent> intents_;
};

struct ActiveSurface final {
    SurfaceFamily family;
    OutputName output;
    FocusOrigin origin;

    friend bool operator==(const ActiveSurface &, const ActiveSurface &) = default;
};

/// Display-free owner of shell popup placement and focus-return state. Returned
/// intents must be mediated by the Qt/window-manager integration layer.
class ShellComposition final {
  public:
    [[nodiscard]] foundation::Result<CompositionTransition>
    observeOutputs(const OutputPlacementSnapshot &snapshot);
    [[nodiscard]] foundation::Result<CompositionTransition>
    observeSettingsOwner(std::optional<AuthoritySnapshot> snapshot);
    [[nodiscard]] foundation::Result<CompositionTransition>
    observeNotificationOwner(std::optional<AuthoritySnapshot> snapshot);

    [[nodiscard]] foundation::Result<CompositionTransition>
    present(SurfaceFamily family, std::optional<OutputName> requestedOutput, FocusOrigin origin);
    [[nodiscard]] foundation::Result<CompositionTransition>
    toggle(SurfaceFamily family, std::optional<OutputName> requestedOutput, FocusOrigin origin);
    [[nodiscard]] foundation::Result<CompositionTransition> dismiss(SurfaceFamily family);
    [[nodiscard]] CompositionTransition escape();

    [[nodiscard]] std::span<const ActiveSurface> activeSurfaces() const noexcept {
        return active_surfaces_;
    }
    [[nodiscard]] const std::optional<SurfaceFamily> &focusedFamily() const noexcept {
        return focused_family_;
    }

  private:
    [[nodiscard]] foundation::Result<CompositionTransition>
    presentImpl(SurfaceFamily family, std::optional<OutputName> requestedOutput, FocusOrigin origin,
                bool toggleRequest);
    [[nodiscard]] CompositionTransition invalidateAll(DismissReason reason);
    [[nodiscard]] CompositionTransition invalidateNotificationDependencies(DismissReason reason,
                                                                           bool availableAfter);
    [[nodiscard]] std::optional<OutputName>
    resolveOutput(SurfaceFamily family, const std::optional<OutputName> &requestedOutput) const;
    [[nodiscard]] bool hasOutput(const OutputName &output) const noexcept;
    [[nodiscard]] bool hasPanel(const OutputName &output) const noexcept;
    [[nodiscard]] std::optional<std::size_t> findFamily(SurfaceFamily family) const noexcept;
    [[nodiscard]] std::optional<std::size_t> findOnOutput(const OutputName &output) const noexcept;

    std::optional<OutputPlacementSnapshot> outputs_;
    std::optional<AuthoritySnapshot> settings_owner_;
    std::optional<AuthoritySnapshot> notification_owner_;
    std::vector<ActiveSurface> active_surfaces_;
    std::optional<SurfaceFamily> focused_family_;
};

} // namespace prismdrake::shell::composition
