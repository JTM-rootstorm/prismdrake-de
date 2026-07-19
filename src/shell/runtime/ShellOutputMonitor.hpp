#pragma once

#include "OutputSurfaceRegistry.hpp"
#include "RandrTopology.hpp"
#include "Result.hpp"

#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

class QScreen;

namespace prismdrake::shell::runtime {

/// Off-GUI-thread RandR monitor reconciled with Qt screens on the GUI thread. X11 events are
/// refresh hints only; observers receive one complete immutable registry generation at a time.
class ShellOutputMonitor final : public QObject {
    Q_OBJECT

  public:
    using SnapshotObserver =
        std::function<foundation::Result<void>(const outputs::SurfaceRegistrySnapshot &)>;
    using FailureObserver = std::function<void(const foundation::Error &, bool fatal)>;

    ShellOutputMonitor(std::string display, SnapshotObserver snapshotObserver,
                       FailureObserver failureObserver, QObject *parent = nullptr);
    ~ShellOutputMonitor() override;

    [[nodiscard]] foundation::Result<void> start();
    void stop() noexcept;
    [[nodiscard]] foundation::Result<void> configure(std::uint32_t panelHeightLogical,
                                                     outputs::PanelPlacementPolicy policy,
                                                     std::uint64_t settingsGeneration);
    void requestRefresh() noexcept;

    /// Applies one already validated native topology with copied Qt observations. This is the
    /// atomic main-thread publication boundary used by the live worker and deterministic tests.
    [[nodiscard]] foundation::Result<void>
    observeTopology(const x11::OutputTopologySnapshot &topology,
                    std::span<const outputs::QtScreenObservation> screens);

  private:
    void workerMain(std::stop_token stopToken) noexcept;
    void publishTopology(x11::RandrTopologySnapshot snapshot) noexcept;
    void publishFailure(foundation::Error error, bool fatal) noexcept;
    void applyTopology(x11::RandrTopologySnapshot snapshot);
    void observeScreen(QScreen &screen);

    std::string display_;
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
};

} // namespace prismdrake::shell::runtime
