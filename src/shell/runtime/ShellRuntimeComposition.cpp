#include "ShellRuntimeComposition.hpp"

#include "OutputSurfaceRegistry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error presentationError(control::ShellRequestFailure failure) {
    switch (failure) {
    case control::ShellRequestFailure::none:
        break;
    case control::ShellRequestFailure::invalid_output:
        return {ErrorCode::validation_error, "The requested shell output is invalid.",
                "Use an output from the current complete topology generation."};
    case control::ShellRequestFailure::output_unavailable:
        return {ErrorCode::not_found, "The requested shell output is unavailable.",
                "Retry against the current complete output topology."};
    case control::ShellRequestFailure::busy:
        return {ErrorCode::cancelled, "A higher-priority shell surface owns focus.",
                "Dismiss the modal shell surface before retrying."};
    case control::ShellRequestFailure::timed_out:
        return {ErrorCode::cancelled, "The shell presentation request timed out.",
                "Retry after the current bounded presentation update completes."};
    case control::ShellRequestFailure::service_stopping:
        return {ErrorCode::cancelled, "The shell presentation service is stopping.",
                "Start a fresh shell owner epoch before retrying."};
    case control::ShellRequestFailure::internal:
        return {ErrorCode::io_error, "The shell presentation request failed.",
                "Restart the shell if the presentation boundary remains unavailable."};
    }
    return {ErrorCode::io_error, "The shell presentation request failed.",
            "Restart the shell if the presentation boundary remains unavailable."};
}

[[nodiscard]] quicksettings::CommandFailure
quickSettingsFailure(control::ShellRequestFailure failure) noexcept {
    switch (failure) {
    case control::ShellRequestFailure::none:
        return quicksettings::CommandFailure::none;
    case control::ShellRequestFailure::busy:
        return quicksettings::CommandFailure::busy;
    case control::ShellRequestFailure::service_stopping:
    case control::ShellRequestFailure::output_unavailable:
        return quicksettings::CommandFailure::unavailable;
    case control::ShellRequestFailure::invalid_output:
    case control::ShellRequestFailure::timed_out:
    case control::ShellRequestFailure::internal:
        return quicksettings::CommandFailure::failed;
    }
    return quicksettings::CommandFailure::failed;
}

[[nodiscard]] control::ShellRequestFailure
transitionFailure(const foundation::Error &error) noexcept {
    switch (error.code) {
    case ErrorCode::not_found:
        return control::ShellRequestFailure::output_unavailable;
    case ErrorCode::invalid_argument:
    case ErrorCode::syntax_error:
    case ErrorCode::validation_error:
        return control::ShellRequestFailure::invalid_output;
    case ErrorCode::cancelled:
        return control::ShellRequestFailure::busy;
    case ErrorCode::io_error:
    case ErrorCode::invalid_environment:
    case ErrorCode::permission_denied:
    case ErrorCode::too_large:
    case ErrorCode::durability_uncertain:
    case ErrorCode::unsupported:
        return control::ShellRequestFailure::internal;
    }
    return control::ShellRequestFailure::internal;
}

} // namespace

Result<void>
ShellRuntimeComposition::observeOutputs(const outputs::SurfaceRegistrySnapshot &snapshot) {
    std::vector<composition::OutputPlacement> placements;
    placements.reserve(snapshot.desktops().size());
    for (const auto &desktop : snapshot.desktops()) {
        auto name = composition::OutputName::create(desktop.outputName);
        if (!name) {
            return Result<void>::failure(name.error());
        }
        bool hasPanel = false;
        for (const auto &panel : snapshot.panels()) {
            if (panel.outputName == desktop.outputName) {
                hasPanel = true;
                break;
            }
        }
        placements.push_back({std::move(name).value(), hasPanel});
    }

    std::optional<composition::OutputName> primary;
    if (snapshot.primaryOutputName()) {
        auto name = composition::OutputName::create(*snapshot.primaryOutputName());
        if (!name) {
            return Result<void>::failure(name.error());
        }
        primary = std::move(name).value();
    }
    auto next = composition::OutputPlacementSnapshot::create(
        snapshot.surfaceGeneration(), std::move(placements), std::move(primary));
    if (!next) {
        return Result<void>::failure(next.error());
    }

    auto candidate = composition_;
    auto transition = candidate.observeOutputs(next.value());
    auto committed = commit(std::move(candidate), std::move(transition));
    if (!committed) {
        return committed;
    }
    outputs_ = std::move(next).value();
    return Result<void>::success();
}

Result<void> ShellRuntimeComposition::observeSettingsOwner(
    std::optional<composition::AuthoritySnapshot> snapshot) {
    const bool available = snapshot.has_value();
    auto candidate = composition_;
    auto transition = candidate.observeSettingsOwner(std::move(snapshot));
    auto committed = commit(std::move(candidate), std::move(transition));
    if (committed) {
        settings_authority_available_ = available;
    }
    return committed;
}

Result<void> ShellRuntimeComposition::observeNotificationOwner(
    std::optional<composition::AuthoritySnapshot> snapshot) {
    const bool available = snapshot.has_value();
    auto candidate = composition_;
    auto transition = candidate.observeNotificationOwner(std::move(snapshot));
    auto committed = commit(std::move(candidate), std::move(transition));
    if (committed) {
        notification_authority_available_ = available;
    }
    return committed;
}

Result<std::optional<composition::OutputName>>
ShellRuntimeComposition::requestedOutput(std::string_view outputName) const {
    if (outputName.empty()) {
        if (!outputs_ || !outputs_->primaryOutput()) {
            return Result<std::optional<composition::OutputName>>::failure(
                presentationError(control::ShellRequestFailure::output_unavailable));
        }
        return Result<std::optional<composition::OutputName>>::success(std::nullopt);
    }
    auto name = composition::OutputName::create(std::string(outputName));
    if (!name) {
        return Result<std::optional<composition::OutputName>>::failure(name.error());
    }
    if (!outputs_) {
        return Result<std::optional<composition::OutputName>>::failure(
            presentationError(control::ShellRequestFailure::output_unavailable));
    }
    for (const auto &output : outputs_->outputs()) {
        if (output.name == name.value()) {
            return Result<std::optional<composition::OutputName>>::success(std::move(name).value());
        }
    }
    return Result<std::optional<composition::OutputName>>::failure(
        presentationError(control::ShellRequestFailure::output_unavailable));
}

control::ShellRequestFailure ShellRuntimeComposition::request(control::ShellAction action,
                                                              std::string_view outputName) {
    auto output = requestedOutput(outputName);
    if (!output) {
        return transitionFailure(output.error());
    }
    if (!settings_authority_available_) {
        return control::ShellRequestFailure::busy;
    }

    const auto origin = composition::FocusOrigin::windowManager();
    auto candidate = composition_;
    switch (action) {
    case control::ShellAction::toggle_launcher: {
        auto transition = candidate.toggle(composition::SurfaceFamily::launcher,
                                           std::move(output).value(), origin);
        return commitRequest(std::move(candidate), std::move(transition));
    }
    case control::ShellAction::open_quick_settings: {
        auto transition = candidate.present(composition::SurfaceFamily::quickSettings,
                                            std::move(output).value(), origin);
        return commitRequest(std::move(candidate), std::move(transition));
    }
    case control::ShellAction::open_notification_center: {
        if (!notification_authority_available_) {
            return control::ShellRequestFailure::busy;
        }
        auto transition = candidate.present(composition::SurfaceFamily::notificationCenter,
                                            std::move(output).value(), origin);
        return commitRequest(std::move(candidate), std::move(transition));
    }
    case control::ShellAction::focus_panel: {
        auto resolved = std::move(output).value();
        if (!resolved) {
            resolved = outputs_->primaryOutput();
        }
        return presentation_port_->focusPanel(*resolved);
    }
    }
    return control::ShellRequestFailure::internal;
}

quicksettings::CommandResult ShellRuntimeComposition::requestOpen() {
    return {quickSettingsFailure(
        request(control::ShellAction::open_notification_center, std::string_view{}))};
}

Result<void> ShellRuntimeComposition::present(composition::SurfaceFamily family,
                                              std::string_view outputName,
                                              composition::FocusOrigin origin) {
    auto output = requestedOutput(outputName);
    if (!output) {
        return Result<void>::failure(output.error());
    }
    auto candidate = composition_;
    auto transition = candidate.present(family, std::move(output).value(), std::move(origin));
    return commit(std::move(candidate), std::move(transition));
}

Result<void> ShellRuntimeComposition::toggle(composition::SurfaceFamily family,
                                             std::string_view outputName,
                                             composition::FocusOrigin origin) {
    auto output = requestedOutput(outputName);
    if (!output) {
        return Result<void>::failure(output.error());
    }
    auto candidate = composition_;
    auto transition = candidate.toggle(family, std::move(output).value(), std::move(origin));
    return commit(std::move(candidate), std::move(transition));
}

Result<void> ShellRuntimeComposition::dismiss(composition::SurfaceFamily family) {
    auto candidate = composition_;
    auto transition = candidate.dismiss(family);
    return commit(std::move(candidate), std::move(transition));
}

Result<void> ShellRuntimeComposition::escape() {
    auto candidate = composition_;
    auto transition = candidate.escape();
    return commit(std::move(candidate),
                  Result<composition::CompositionTransition>::success(std::move(transition)));
}

control::ShellRequestFailure
ShellRuntimeComposition::applyPresentation(const composition::CompositionTransition &transition) {
    if (transition.disposition() == composition::TransitionDisposition::blockedByHigherPriority) {
        const auto focused = presentation_port_->apply(transition.intents());
        return focused == control::ShellRequestFailure::none ? control::ShellRequestFailure::busy
                                                             : focused;
    }
    return presentation_port_->apply(transition.intents());
}

Result<void>
ShellRuntimeComposition::commit(composition::ShellComposition candidate,
                                Result<composition::CompositionTransition> transition) {
    if (!transition) {
        return Result<void>::failure(transition.error());
    }
    const auto failure = applyPresentation(transition.value());
    if (failure != control::ShellRequestFailure::none) {
        return Result<void>::failure(presentationError(failure));
    }
    composition_ = std::move(candidate);
    return Result<void>::success();
}

control::ShellRequestFailure
ShellRuntimeComposition::commitRequest(composition::ShellComposition candidate,
                                       Result<composition::CompositionTransition> transition) {
    if (!transition) {
        return transitionFailure(transition.error());
    }
    const auto failure = applyPresentation(transition.value());
    if (failure != control::ShellRequestFailure::none) {
        return failure;
    }
    composition_ = std::move(candidate);
    return control::ShellRequestFailure::none;
}

} // namespace prismdrake::shell::runtime
