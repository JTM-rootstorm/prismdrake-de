#pragma once

#include "OutputSurfaceRegistry.hpp"
#include "RandrTopology.hpp"
#include "Result.hpp"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

class QScreen;

namespace prismdrake::shell::runtime {

struct ShellOutputMonitorEventDrain final {
    bool refreshRequired;
    bool followUpRequired;
};

/// Worker-owned reply-bearing RandR operations. The production implementation owns one X11
/// connection; tests inject the same cancellation and event boundary without a display server.
class ShellOutputMonitorOperations {
  public:
    virtual ~ShellOutputMonitorOperations() = default;
    /// Performs reply-bearing setup after this object has been published to stop().
    [[nodiscard]] virtual foundation::Result<void> initialize() = 0;
    [[nodiscard]] virtual foundation::Result<x11::RandrTopologySnapshot> query() = 0;
    [[nodiscard]] virtual int eventFileDescriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result<ShellOutputMonitorEventDrain> drainEvents() = 0;
    virtual void cancelWaits() noexcept = 0;
};

/// Off-GUI-thread RandR monitor reconciled with Qt screens on the GUI thread. X11 events are
/// refresh hints only; observers receive one complete immutable registry generation at a time.
class ShellOutputMonitor final : public QObject {
    Q_OBJECT

  public:
    using SnapshotObserver =
        std::function<foundation::Result<void>(const outputs::SurfaceRegistrySnapshot &)>;
    using FailureObserver = std::function<void(const foundation::Error &, bool fatal)>;
    /// Creates only the connection-owning operations object. Reply-bearing protocol setup belongs
    /// in ShellOutputMonitorOperations::initialize(), after stop() can reach cancelWaits().
    using OperationsFactory =
        std::function<foundation::Result<std::unique_ptr<ShellOutputMonitorOperations>>()>;

    ShellOutputMonitor(std::string display, SnapshotObserver snapshotObserver,
                       FailureObserver failureObserver, QObject *parent = nullptr);
    ShellOutputMonitor(OperationsFactory operationsFactory, SnapshotObserver snapshotObserver,
                       FailureObserver failureObserver, QObject *parent = nullptr);
    ~ShellOutputMonitor() override;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] foundation::Result<void> configure(std::uint32_t panelHeightLogical,
                                                     outputs::PanelPlacementPolicy policy,
                                                     std::uint64_t settingsGeneration);
    void beginSettingsOwnerEpoch() noexcept;
    void requestRefresh() noexcept;

    /// Applies one already validated native topology with copied Qt observations. This is the
    /// atomic main-thread publication boundary used by the live worker and deterministic tests.
    [[nodiscard]] foundation::Result<void>
    observeTopology(const x11::OutputTopologySnapshot &topology,
                    std::span<const outputs::QtScreenObservation> screens);

  private:
    struct PublicationState;
    void workerMain(std::stop_token stopToken) noexcept;
    void publishTopology(x11::RandrTopologySnapshot snapshot) noexcept;
    void publishFailure(foundation::Error error, bool fatal) noexcept;
    void applyTopology(x11::RandrTopologySnapshot snapshot);
    void observeScreen(QScreen &screen);
    void disconnectScreenObservers() noexcept;
    void queuePublication(const std::shared_ptr<PublicationState> &state) noexcept;
    [[nodiscard]] std::shared_ptr<PublicationState> publicationState() const noexcept;

    OperationsFactory operations_factory_;
    bool observe_qt_screens_{true};
    SnapshotObserver snapshot_observer_;
    FailureObserver failure_observer_;
    std::jthread worker_;
    int wake_descriptor_{-1};
    std::optional<outputs::OutputSurfaceRegistry> registry_;
    outputs::PanelPlacementPolicy panel_policy_{outputs::PanelOutputPolicy::primary, {}};
    std::optional<outputs::ReconciledTopologySnapshot> topology_;
    std::uint64_t generation_{0U};
    std::uint64_t settings_generation_{0U};
    std::atomic_bool running_{false};
    std::atomic<ShellOutputMonitorOperations *> operations_{nullptr};
    std::mutex operations_mutex_;
    mutable std::mutex publication_state_mutex_;
    std::shared_ptr<PublicationState> publication_state_;
};

} // namespace prismdrake::shell::runtime
