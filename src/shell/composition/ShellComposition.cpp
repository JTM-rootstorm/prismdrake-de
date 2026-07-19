#include "ShellComposition.hpp"

#include "OutputTopology.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace prismdrake::shell::composition {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value> [[nodiscard]] Result<Value> invalidState(std::string message) {
    return Result<Value>::failure({ErrorCode::validation_error, std::move(message),
                                   "Provide one complete newer typed composition observation."});
}

template <typename Value> [[nodiscard]] Result<Value> unavailable(std::string message) {
    return Result<Value>::failure(
        {ErrorCode::not_found, std::move(message),
         "Wait for the required owner and output observations, then retry the typed request."});
}

[[nodiscard]] bool validRole(FocusControlRole role) noexcept {
    switch (role) {
    case FocusControlRole::panelLauncher:
    case FocusControlRole::panelNotificationCenter:
    case FocusControlRole::panelQuickSettings:
    case FocusControlRole::panelTaskGroup:
    case FocusControlRole::launcherResult:
    case FocusControlRole::notificationCard:
    case FocusControlRole::logoutControl:
        return true;
    }
    return false;
}

[[nodiscard]] bool validFamily(SurfaceFamily family) noexcept {
    switch (family) {
    case SurfaceFamily::logoutConfirmation:
    case SurfaceFamily::actionMenu:
    case SurfaceFamily::quickSettings:
    case SurfaceFamily::notificationCenter:
    case SurfaceFamily::launcher:
    case SurfaceFamily::transientNotificationAction:
        return true;
    }
    return false;
}

[[nodiscard]] unsigned priority(SurfaceFamily family) noexcept {
    switch (family) {
    case SurfaceFamily::logoutConfirmation:
        return 6U;
    case SurfaceFamily::actionMenu:
        return 5U;
    case SurfaceFamily::quickSettings:
    case SurfaceFamily::notificationCenter:
        return 4U;
    case SurfaceFamily::launcher:
        return 3U;
    case SurfaceFamily::transientNotificationAction:
        return 2U;
    }
    return 0U;
}

[[nodiscard]] bool primaryBound(SurfaceFamily family) noexcept {
    return family == SurfaceFamily::logoutConfirmation ||
           family == SurfaceFamily::transientNotificationAction;
}

[[nodiscard]] bool needsNotificationOwner(SurfaceFamily family) noexcept {
    return family == SurfaceFamily::notificationCenter ||
           family == SurfaceFamily::transientNotificationAction;
}

[[nodiscard]] bool notificationDependent(SurfaceFamily family) noexcept {
    return needsNotificationOwner(family);
}

[[nodiscard]] bool sameAuthorityEpoch(const AuthoritySnapshot &left,
                                      const AuthoritySnapshot &right) noexcept {
    return left.epoch == right.epoch;
}

void appendFocusRecovery(std::vector<CompositionIntent> &intents, const FocusOrigin &origin) {
    if (origin.kind() == FocusOriginKind::control) {
        intents.emplace_back(RestoreControlFocusIntent{origin});
    } else {
        intents.emplace_back(ReleaseFocusToWindowManagerIntent{});
    }
}

} // namespace

Result<OutputName> OutputName::create(std::string value) {
    if (!x11::isStableOutputName(value)) {
        return invalidState<OutputName>("The composition output name is invalid.");
    }
    return Result<OutputName>::success(OutputName{std::move(value)});
}

Result<FocusControlId> FocusControlId::create(std::string value) {
    if (!x11::isStableOutputName(value)) {
        return invalidState<FocusControlId>("The opaque focus control identity is invalid.");
    }
    return Result<FocusControlId>::success(FocusControlId{std::move(value)});
}

FocusOrigin::FocusOrigin(FocusOriginKind kind, std::optional<OutputName> output,
                         std::optional<FocusControlRole> role,
                         std::optional<FocusControlId> controlId, bool equivalent) noexcept
    : kind_(kind), output_(std::move(output)), role_(role), control_id_(std::move(controlId)),
      equivalent_(equivalent) {}

Result<FocusOrigin> FocusOrigin::control(OutputName output, FocusControlRole role,
                                         FocusControlId controlId) {
    if (!validRole(role)) {
        return invalidState<FocusOrigin>("The focus control role is invalid.");
    }
    return Result<FocusOrigin>::success(FocusOrigin{FocusOriginKind::control, std::move(output),
                                                    role, std::move(controlId), false});
}

FocusOrigin FocusOrigin::windowManager() noexcept {
    return {FocusOriginKind::windowManager, std::nullopt, std::nullopt, std::nullopt, false};
}

FocusOrigin FocusOrigin::transferredTo(OutputName output) const {
    if (kind_ != FocusOriginKind::control) {
        return *this;
    }
    return {kind_, std::move(output), role_, control_id_, true};
}

OutputPlacementSnapshot::OutputPlacementSnapshot(std::uint64_t generation,
                                                 std::vector<OutputPlacement> outputs,
                                                 std::optional<OutputName> primaryOutput) noexcept
    : generation_(generation), outputs_(std::move(outputs)),
      primary_output_(std::move(primaryOutput)) {}

Result<OutputPlacementSnapshot>
OutputPlacementSnapshot::create(std::uint64_t generation, std::vector<OutputPlacement> outputs,
                                std::optional<OutputName> primaryOutput) {
    if (generation == 0U || outputs.size() > maximumCompositionOutputs) {
        return invalidState<OutputPlacementSnapshot>("The output placement generation is invalid.");
    }
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        if (std::find_if(outputs.begin(), outputs.begin() + static_cast<std::ptrdiff_t>(index),
                         [&outputs, index](const auto &candidate) {
                             return candidate.name == outputs[index].name;
                         }) != outputs.begin() + static_cast<std::ptrdiff_t>(index)) {
            return invalidState<OutputPlacementSnapshot>(
                "The complete output placement contains a duplicate name.");
        }
    }
    if (primaryOutput &&
        std::none_of(outputs.begin(), outputs.end(), [&primaryOutput](const auto &output) {
            return output.name == *primaryOutput;
        })) {
        return invalidState<OutputPlacementSnapshot>(
            "The primary output is absent from the complete placement.");
    }
    return Result<OutputPlacementSnapshot>::success(
        OutputPlacementSnapshot{generation, std::move(outputs), std::move(primaryOutput)});
}

Result<OwnerEpoch> OwnerEpoch::create(std::uint64_t value) {
    if (value == 0U) {
        return invalidState<OwnerEpoch>("The owner epoch is invalid.");
    }
    return Result<OwnerEpoch>::success(OwnerEpoch{value});
}

Result<OwnerGeneration> OwnerGeneration::create(std::uint64_t value) {
    if (value == 0U) {
        return invalidState<OwnerGeneration>("The owner generation is invalid.");
    }
    return Result<OwnerGeneration>::success(OwnerGeneration{value});
}

bool ShellComposition::hasOutput(const OutputName &output) const noexcept {
    return outputs_ &&
           std::any_of(outputs_->outputs().begin(), outputs_->outputs().end(),
                       [&output](const auto &candidate) { return candidate.name == output; });
}

bool ShellComposition::hasPanel(const OutputName &output) const noexcept {
    if (!outputs_) {
        return false;
    }
    const auto found =
        std::find_if(outputs_->outputs().begin(), outputs_->outputs().end(),
                     [&output](const auto &candidate) { return candidate.name == output; });
    return found != outputs_->outputs().end() && found->hasPanel;
}

std::optional<std::size_t> ShellComposition::findFamily(SurfaceFamily family) const noexcept {
    const auto found =
        std::find_if(active_surfaces_.begin(), active_surfaces_.end(),
                     [family](const auto &surface) { return surface.family == family; });
    if (found == active_surfaces_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(active_surfaces_.begin(), found));
}

std::optional<std::size_t> ShellComposition::findOnOutput(const OutputName &output) const noexcept {
    const auto found =
        std::find_if(active_surfaces_.begin(), active_surfaces_.end(),
                     [&output](const auto &surface) { return surface.output == output; });
    if (found == active_surfaces_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(active_surfaces_.begin(), found));
}

std::optional<OutputName>
ShellComposition::resolveOutput(SurfaceFamily family,
                                const std::optional<OutputName> &requestedOutput) const {
    if (!outputs_) {
        return std::nullopt;
    }
    if (!primaryBound(family) && requestedOutput) {
        return hasOutput(*requestedOutput) ? requestedOutput : std::nullopt;
    }
    return outputs_->primaryOutput();
}

Result<CompositionTransition>
ShellComposition::observeOutputs(const OutputPlacementSnapshot &snapshot) {
    if (outputs_ && snapshot.generation() <= outputs_->generation()) {
        return invalidState<CompositionTransition>("The output placement generation is stale.");
    }

    outputs_ = snapshot;
    auto previousSurfaces = std::move(active_surfaces_);
    active_surfaces_.clear();
    CompositionTransition transition;

    std::stable_sort(previousSurfaces.begin(), previousSurfaces.end(),
                     [](const auto &left, const auto &right) {
                         return priority(left.family) > priority(right.family);
                     });

    bool focusedSurvived = false;
    for (auto &surface : previousSurfaces) {
        const bool outputStillPresent = hasOutput(surface.output);
        std::optional<OutputName> target;
        if (outputStillPresent &&
            (!primaryBound(surface.family) ||
             (outputs_->primaryOutput() && surface.output == *outputs_->primaryOutput()))) {
            target = surface.output;
        } else {
            target = outputs_->primaryOutput();
        }

        if (!target) {
            transition.intents_.emplace_back(
                DismissSurfaceIntent{surface.family, surface.output, DismissReason::outputRemoved});
            continue;
        }

        if (const auto conflict = findOnOutput(*target); conflict) {
            transition.intents_.emplace_back(
                DismissSurfaceIntent{surface.family, surface.output, DismissReason::conflict});
            continue;
        }

        if (surface.origin.kind() == FocusOriginKind::control && surface.origin.output() &&
            !hasOutput(*surface.origin.output())) {
            surface.origin = hasPanel(*target) ? surface.origin.transferredTo(*target)
                                               : FocusOrigin::windowManager();
        }
        if (surface.output != *target) {
            transition.intents_.emplace_back(
                MoveSurfaceIntent{surface.family, surface.output, *target});
            surface.output = *target;
        }
        if (focused_family_ == surface.family) {
            focusedSurvived = true;
            transition.intents_.emplace_back(FocusSurfaceIntent{surface.family, surface.output});
        }
        active_surfaces_.push_back(std::move(surface));
    }

    if (focused_family_ && !focusedSurvived) {
        focused_family_.reset();
        transition.intents_.emplace_back(ReleaseFocusToWindowManagerIntent{});
    }
    return Result<CompositionTransition>::success(std::move(transition));
}

CompositionTransition ShellComposition::invalidateAll(DismissReason reason) {
    CompositionTransition transition;
    for (const auto &surface : active_surfaces_) {
        transition.intents_.emplace_back(
            DismissSurfaceIntent{surface.family, surface.output, reason});
    }
    if (focused_family_) {
        transition.intents_.emplace_back(ReleaseFocusToWindowManagerIntent{});
    }
    active_surfaces_.clear();
    focused_family_.reset();
    return transition;
}

CompositionTransition ShellComposition::invalidateNotificationDependencies(DismissReason reason,
                                                                           bool availableAfter) {
    CompositionTransition transition;
    std::optional<FocusOrigin> focusedOrigin;
    for (const auto &surface : active_surfaces_) {
        if (notificationDependent(surface.family)) {
            transition.intents_.emplace_back(
                DismissSurfaceIntent{surface.family, surface.output, reason});
            if (focused_family_ == surface.family) {
                focusedOrigin = surface.origin;
            }
        }
    }
    std::erase_if(active_surfaces_,
                  [](const auto &surface) { return notificationDependent(surface.family); });
    if (focusedOrigin) {
        focused_family_.reset();
        appendFocusRecovery(transition.intents_, *focusedOrigin);
    }
    transition.intents_.emplace_back(DependencyAvailabilityIntent{
        SurfaceFamily::quickSettings, OptionalDependency::notificationAuthority, availableAfter});
    return transition;
}

Result<CompositionTransition>
ShellComposition::observeSettingsOwner(std::optional<AuthoritySnapshot> snapshot) {
    if (!snapshot) {
        if (!settings_owner_) {
            return Result<CompositionTransition>::success({});
        }
        settings_owner_.reset();
        return Result<CompositionTransition>::success(
            invalidateAll(DismissReason::settingsOwnerChanged));
    }
    if (settings_owner_ && sameAuthorityEpoch(*settings_owner_, *snapshot) &&
        snapshot->generation.value() <= settings_owner_->generation.value()) {
        return invalidState<CompositionTransition>("The settings owner generation is stale.");
    }
    const bool changedEpoch = settings_owner_ && !sameAuthorityEpoch(*settings_owner_, *snapshot);
    settings_owner_ = snapshot;
    if (changedEpoch) {
        return Result<CompositionTransition>::success(
            invalidateAll(DismissReason::settingsOwnerChanged));
    }
    return Result<CompositionTransition>::success({});
}

Result<CompositionTransition>
ShellComposition::observeNotificationOwner(std::optional<AuthoritySnapshot> snapshot) {
    if (!snapshot) {
        if (!notification_owner_) {
            return Result<CompositionTransition>::success({});
        }
        notification_owner_.reset();
        return Result<CompositionTransition>::success(
            invalidateNotificationDependencies(DismissReason::notificationOwnerChanged, false));
    }
    if (notification_owner_ && sameAuthorityEpoch(*notification_owner_, *snapshot) &&
        snapshot->generation.value() <= notification_owner_->generation.value()) {
        return invalidState<CompositionTransition>("The notification owner generation is stale.");
    }
    const bool changedEpoch =
        notification_owner_ && !sameAuthorityEpoch(*notification_owner_, *snapshot);
    notification_owner_ = snapshot;
    auto transition =
        changedEpoch
            ? invalidateNotificationDependencies(DismissReason::notificationOwnerChanged, true)
            : CompositionTransition{};
    if (!changedEpoch) {
        transition.intents_.emplace_back(DependencyAvailabilityIntent{
            SurfaceFamily::quickSettings, OptionalDependency::notificationAuthority, true});
    }
    return Result<CompositionTransition>::success(std::move(transition));
}

Result<CompositionTransition> ShellComposition::present(SurfaceFamily family,
                                                        std::optional<OutputName> requestedOutput,
                                                        FocusOrigin origin) {
    return presentImpl(family, std::move(requestedOutput), std::move(origin), false);
}

Result<CompositionTransition> ShellComposition::toggle(SurfaceFamily family,
                                                       std::optional<OutputName> requestedOutput,
                                                       FocusOrigin origin) {
    return presentImpl(family, std::move(requestedOutput), std::move(origin), true);
}

Result<CompositionTransition>
ShellComposition::presentImpl(SurfaceFamily family, std::optional<OutputName> requestedOutput,
                              FocusOrigin origin, bool toggleRequest) {
    if (!validFamily(family)) {
        return invalidState<CompositionTransition>("The shell surface family is invalid.");
    }
    if (!settings_owner_) {
        return unavailable<CompositionTransition>("The settings authority is unavailable.");
    }
    if (needsNotificationOwner(family) && !notification_owner_) {
        return unavailable<CompositionTransition>("The notification authority is unavailable.");
    }
    if (requestedOutput && !hasOutput(*requestedOutput)) {
        return unavailable<CompositionTransition>("The requested shell output is unavailable.");
    }
    if (origin.kind() == FocusOriginKind::control &&
        (!origin.output() || !hasOutput(*origin.output()))) {
        return unavailable<CompositionTransition>("The invoking focus control is unavailable.");
    }
    const auto target = resolveOutput(family, requestedOutput);
    if (!target) {
        return unavailable<CompositionTransition>("No primary shell output is available.");
    }

    CompositionTransition transition;
    if (family != SurfaceFamily::logoutConfirmation) {
        if (const auto modal = findFamily(SurfaceFamily::logoutConfirmation); modal) {
            transition.disposition_ = TransitionDisposition::blockedByHigherPriority;
            transition.intents_.emplace_back(FocusSurfaceIntent{SurfaceFamily::logoutConfirmation,
                                                                active_surfaces_[*modal].output});
            focused_family_ = SurfaceFamily::logoutConfirmation;
            return Result<CompositionTransition>::success(std::move(transition));
        }
    }
    if (const auto existingFamily = findFamily(family); existingFamily) {
        auto &existing = active_surfaces_[*existingFamily];
        if (toggleRequest && existing.output == *target) {
            transition.intents_.emplace_back(
                DismissSurfaceIntent{family, existing.output, DismissReason::explicitRequest});
            if (focused_family_ == family) {
                appendFocusRecovery(transition.intents_, existing.origin);
                focused_family_.reset();
            }
            active_surfaces_.erase(active_surfaces_.begin() +
                                   static_cast<std::ptrdiff_t>(*existingFamily));
            return Result<CompositionTransition>::success(std::move(transition));
        }
    }

    if (family == SurfaceFamily::logoutConfirmation) {
        for (const auto &surface : active_surfaces_) {
            if (surface.family != family) {
                transition.intents_.emplace_back(
                    DismissSurfaceIntent{surface.family, surface.output, DismissReason::conflict});
            }
        }
        std::erase_if(active_surfaces_,
                      [family](const auto &surface) { return surface.family != family; });
    }

    if (const auto conflict = findOnOutput(*target);
        conflict && active_surfaces_[*conflict].family != family) {
        const auto existingPriority = priority(active_surfaces_[*conflict].family);
        if (existingPriority > priority(family)) {
            transition.disposition_ = TransitionDisposition::blockedByHigherPriority;
            transition.intents_.emplace_back(FocusSurfaceIntent{
                active_surfaces_[*conflict].family, active_surfaces_[*conflict].output});
            focused_family_ = active_surfaces_[*conflict].family;
            return Result<CompositionTransition>::success(std::move(transition));
        }
        const auto dismissed = active_surfaces_[*conflict];
        transition.intents_.emplace_back(
            DismissSurfaceIntent{dismissed.family, dismissed.output, DismissReason::conflict});
        active_surfaces_.erase(active_surfaces_.begin() + static_cast<std::ptrdiff_t>(*conflict));
    }

    if (const auto existingFamily = findFamily(family); existingFamily) {
        auto &existing = active_surfaces_[*existingFamily];
        if (existing.output != *target) {
            transition.intents_.emplace_back(MoveSurfaceIntent{family, existing.output, *target});
            existing.output = *target;
        }
        existing.origin = std::move(origin);
    } else {
        active_surfaces_.push_back({family, *target, std::move(origin)});
        transition.intents_.emplace_back(PresentSurfaceIntent{family, *target});
    }
    focused_family_ = family;
    transition.intents_.emplace_back(FocusSurfaceIntent{family, *target});
    return Result<CompositionTransition>::success(std::move(transition));
}

Result<CompositionTransition> ShellComposition::dismiss(SurfaceFamily family) {
    if (!validFamily(family)) {
        return invalidState<CompositionTransition>("The shell surface family is invalid.");
    }
    CompositionTransition transition;
    const auto found = findFamily(family);
    if (!found) {
        return Result<CompositionTransition>::success(std::move(transition));
    }
    const auto surface = active_surfaces_[*found];
    transition.intents_.emplace_back(
        DismissSurfaceIntent{family, surface.output, DismissReason::explicitRequest});
    active_surfaces_.erase(active_surfaces_.begin() + static_cast<std::ptrdiff_t>(*found));
    if (focused_family_ == family) {
        appendFocusRecovery(transition.intents_, surface.origin);
        focused_family_.reset();
    }
    return Result<CompositionTransition>::success(std::move(transition));
}

CompositionTransition ShellComposition::escape() {
    CompositionTransition transition;
    if (!focused_family_) {
        transition.intents_.emplace_back(ReleaseFocusToWindowManagerIntent{});
        return transition;
    }
    if (*focused_family_ == SurfaceFamily::logoutConfirmation) {
        transition.intents_.emplace_back(CancelLogoutIntent{});
        return transition;
    }
    const auto found = findFamily(*focused_family_);
    if (!found) {
        focused_family_.reset();
        transition.intents_.emplace_back(ReleaseFocusToWindowManagerIntent{});
        return transition;
    }
    const auto surface = active_surfaces_[*found];
    transition.intents_.emplace_back(
        DismissSurfaceIntent{surface.family, surface.output, DismissReason::escape});
    active_surfaces_.erase(active_surfaces_.begin() + static_cast<std::ptrdiff_t>(*found));
    focused_family_.reset();
    appendFocusRecovery(transition.intents_, surface.origin);
    return transition;
}

} // namespace prismdrake::shell::composition
