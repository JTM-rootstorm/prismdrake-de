#pragma once

#include "QuickSettingsModel.hpp"
#include "Result.hpp"
#include "ShellComposition.hpp"
#include "ShellControlService.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace prismdrake::shell::outputs {
class SurfaceRegistrySnapshot;
}

namespace prismdrake::shell::runtime {

/// Narrow presentation boundary between the display-free composition reducer and Qt windows.
/// Implementations consume one complete transition in order and keep raw X11/D-Bus identities
/// outside QML.
class ShellRuntimePresentationPort {
  public:
    virtual ~ShellRuntimePresentationPort() = default;
    [[nodiscard]] virtual control::ShellRequestFailure
    apply(std::span<const composition::CompositionIntent> intents) = 0;
    [[nodiscard]] virtual control::ShellRequestFailure
    focusPanel(const composition::OutputName &output) = 0;
};

/// Display-free runtime owner of output placement, authority epochs, popup routing, and
/// focus-return policy. Shell1 and quick settings share this exact router.
class ShellRuntimeComposition final : public control::ShellPresentationPort,
                                      public quicksettings::NotificationCenterPort {
  public:
    explicit ShellRuntimeComposition(ShellRuntimePresentationPort &presentationPort) noexcept
        : presentation_port_(&presentationPort) {}

    [[nodiscard]] foundation::Result<void>
    observeOutputs(const outputs::SurfaceRegistrySnapshot &snapshot);
    [[nodiscard]] foundation::Result<void>
    observeSettingsOwner(std::optional<composition::AuthoritySnapshot> snapshot);
    [[nodiscard]] foundation::Result<void>
    observeNotificationOwner(std::optional<composition::AuthoritySnapshot> snapshot);

    [[nodiscard]] control::ShellRequestFailure request(control::ShellAction action,
                                                       std::string_view outputName) override;
    [[nodiscard]] quicksettings::CommandResult requestOpen() override;

    [[nodiscard]] foundation::Result<void> present(composition::SurfaceFamily family,
                                                   std::string_view outputName,
                                                   composition::FocusOrigin origin);
    [[nodiscard]] foundation::Result<void> dismiss(composition::SurfaceFamily family);
    [[nodiscard]] foundation::Result<void> escape();

    [[nodiscard]] const composition::ShellComposition &state() const noexcept {
        return composition_;
    }

  private:
    [[nodiscard]] foundation::Result<std::optional<composition::OutputName>>
    requestedOutput(std::string_view outputName) const;
    [[nodiscard]] foundation::Result<void>
    apply(foundation::Result<composition::CompositionTransition> transition);
    [[nodiscard]] foundation::Result<void>
    apply(const composition::CompositionTransition &transition);
    [[nodiscard]] control::ShellRequestFailure mapRequestResult(const foundation::Result<void> &);

    ShellRuntimePresentationPort *presentation_port_;
    composition::ShellComposition composition_;
    std::optional<composition::OutputPlacementSnapshot> outputs_;
    std::optional<control::ShellRequestFailure> last_presentation_failure_;
};

} // namespace prismdrake::shell::runtime
