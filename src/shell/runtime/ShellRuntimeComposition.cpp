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
        return {ErrorCode::invalid_argument, "The requested shell output is invalid.",
                "Use an output from the current complete topology generation."};
    case control::ShellRequestFailure::output_unavailable:
        return {ErrorCode::cancelled, "The requested shell output is unavailable.",
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
        snapshot.topologyGeneration(), std::move(placements), std::move(primary));
    if (!next) {
        return Result<void>::failure(next.error());
    }
    auto transition = composition_.observeOutputs(next.value());
    if (!transition) {
        return Result<void>::failure(transition.error());
    }
    auto applied = apply(transition.value());
    if (!applied) {
        return applied;
    }
    outputs_ = std::move(next).value();
    return Result<void>::success();
}

Result<void> ShellRuntimeComposition::observeSettingsOwner(
    std::optional<composition::AuthoritySnapshot> snapshot) {
    return apply(composition_.observeSettingsOwner(std::move(snapshot)));
}

Result<void> ShellRuntimeComposition::observeNotificationOwner(
    std::optional<composition::AuthoritySnapshot> snapshot) {
    return apply(composition_.observeNotificationOwner(std::move(snapshot)));
}

Result<std::optional<composition::OutputName>>
ShellRuntimeComposition::requestedOutput(std::string_view outputName) const {
    if (outputName.empty()) {
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
    bool present = false;
    for (const auto &output : outputs_->outputs()) {
        if (output.name == name.value()) {
            present = true;
            break;
        }
    }
    if (!present) {
        return Result<std::optional<composition::OutputName>>::failure(
            presentationError(control::ShellRequestFailure::output_unavailable));
    }
    return Result<std::optional<composition::OutputName>>::success(std::move(name).value());
}

control::ShellRequestFailure ShellRuntimeComposition::request(control::ShellAction action,
                                                              std::string_view outputName) {
    auto output = requestedOutput(outputName);
    if (!output) {
        return outputName.empty() ? control::ShellRequestFailure::output_unavailable
                                  : control::ShellRequestFailure::output_unavailable;
    }
    const auto origin = composition::FocusOrigin::windowManager();
    switch (action) {
    case control::ShellAction::toggle_launcher:
        return mapRequestResult(apply(composition_.toggle(composition::SurfaceFamily::launcher,
                                                          std::move(output).value(), origin)));
    case control::ShellAction::open_quick_settings:
        return mapRequestResult(apply(composition_.present(
            composition::SurfaceFamily::quickSettings, std::move(output).value(), origin)));
    case control::ShellAction::open_notification_center:
        return mapRequestResult(apply(composition_.present(
            composition::SurfaceFamily::notificationCenter, std::move(output).value(), origin)));
    case control::ShellAction::focus_panel: {
        auto resolved = std::move(output).value();
        if (!resolved) {
            if (!outputs_ || !outputs_->primaryOutput()) {
                return control::ShellRequestFailure::output_unavailable;
            }
            resolved = outputs_->primaryOutput();
        }
        return presentation_port_->focusPanel(*resolved);
    }
    }
    return control::ShellRequestFailure::internal;
}

quicksettings::CommandResult ShellRuntimeComposition::requestOpen() {
    const auto failure = request(control::ShellAction::open_notification_center, {});
    return {quickSettingsFailure(failure)};
}

Result<void> ShellRuntimeComposition::present(composition::SurfaceFamily family,
                                              std::string_view outputName,
                                              composition::FocusOrigin origin) {
    auto output = requestedOutput(outputName);
    if (!output) {
        return Result<void>::failure(output.error());
    }
    return apply(composition_.present(family, std::move(output).value(), std::move(origin)));
}

Result<void> ShellRuntimeComposition::dismiss(composition::SurfaceFamily family) {
    return apply(composition_.dismiss(family));
}

Result<void> ShellRuntimeComposition::escape() { return apply(composition_.escape()); }

Result<void> ShellRuntimeComposition::apply(Result<composition::CompositionTransition> transition) {
    if (!transition) {
        return Result<void>::failure(transition.error());
    }
    return apply(transition.value());
}

Result<void> ShellRuntimeComposition::apply(const composition::CompositionTransition &transition) {
    if (transition.disposition() == composition::TransitionDisposition::blockedByHigherPriority) {
        return Result<void>::failure(presentationError(control::ShellRequestFailure::busy));
    }
    const auto failure = presentation_port_->apply(transition.intents());
    if (failure != control::ShellRequestFailure::none) {
        last_presentation_failure_ = failure;
        return Result<void>::failure(presentationError(failure));
    }
    return Result<void>::success();
}

control::ShellRequestFailure ShellRuntimeComposition::mapRequestResult(const Result<void> &result) {
    if (result) {
        last_presentation_failure_.reset();
        return control::ShellRequestFailure::none;
    }
    if (last_presentation_failure_) {
        const auto failure = *last_presentation_failure_;
        last_presentation_failure_.reset();
        return failure;
    }
    switch (result.error().code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::validation_error:
        return control::ShellRequestFailure::invalid_output;
    case ErrorCode::cancelled:
        return control::ShellRequestFailure::busy;
    case ErrorCode::syntax_error:
    case ErrorCode::io_error:
    case ErrorCode::invalid_environment:
    case ErrorCode::not_found:
    case ErrorCode::too_large:
    case ErrorCode::permission_denied:
    case ErrorCode::durability_uncertain:
    case ErrorCode::unsupported:
        return control::ShellRequestFailure::internal;
    }
    return control::ShellRequestFailure::internal;
}

} // namespace prismdrake::shell::runtime
