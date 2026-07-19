#pragma once

#include "NotificationCenterClient.hpp"
#include "NotificationCenterPresentation.hpp"
#include "QuickSettingsModel.hpp"
#include "Result.hpp"
#include "ShellComposition.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <optional>

namespace prismdrake::shell::runtime {

/// Owner-aware bridge from NotificationCenter1 to shell-only presentation and quick settings.
/// Mutation request identities and record identities remain private C++ values.
class NotificationRuntimeAdapter final : public QObject,
                                         public quicksettings::DoNotDisturbControlPort {
    Q_OBJECT

  public:
    using AuthorityObserver = std::function<void(std::optional<composition::AuthoritySnapshot>)>;
    using FailureObserver = std::function<void(const foundation::Error &)>;

    NotificationRuntimeAdapter(quicksettings::QuickSettingsModel &quickSettings,
                               AuthorityObserver authorityObserver,
                               FailureObserver failureObserver = {}, QObject *parent = nullptr);
    ~NotificationRuntimeAdapter() override;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;

    [[nodiscard]] quicksettings::CommandResult
    requestDoNotDisturb(bool enabled, quicksettings::CommandRequestId requestId) override;

    [[nodiscard]] notificationcenter::NotificationCenterPresentation &presentation() noexcept {
        return presentation_;
    }

  private:
    void handleSnapshot();
    void handleState();
    void handleMutation(notificationcenter::NotificationCenterClient::Mutation mutation,
                        bool confirmed);
    void loseOwner() noexcept;
    void report(const foundation::Error &error) const;

    quicksettings::QuickSettingsModel *quick_settings_;
    AuthorityObserver authority_observer_;
    FailureObserver failure_observer_;
    notificationcenter::NotificationCenterClient client_;
    notificationcenter::NotificationCenterPresentation presentation_;
    std::optional<quicksettings::CommandRequestId> pending_dnd_;
    bool pending_dnd_confirmed_{false};
    bool owner_available_{false};
    std::uint64_t owner_epoch_{0U};
};

} // namespace prismdrake::shell::runtime
