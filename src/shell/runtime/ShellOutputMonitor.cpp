#include "ShellOutputMonitor.hpp"

#include "OutputTopology.hpp"
#include "RootEventStream.hpp"
#include "X11Connection.hpp"

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QScreen>

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

using foundation::ErrorCode;
using foundation::Result;

[[nodiscard]] foundation::Error monitorError() {
    return {ErrorCode::io_error, "The shell output monitor lost its X11 event boundary.",
            "Restart the exact shell child and rebuild all output surfaces."};
}

[[nodiscard]] foundation::Error invalidConfiguration() {
    return {ErrorCode::validation_error, "The shell output policy is invalid.",
            "Use a positive bounded panel height and a complete panel output policy."};
}

[[nodiscard]] bool needsRefresh(const x11::RootEvent &event) noexcept {
    return std::holds_alternative<x11::OutputTopologyRefreshHint>(event) ||
           std::holds_alternative<x11::RootGeometryHint>(event) ||
           std::holds_alternative<x11::ProtocolErrorHint>(event);
}

} // namespace

ShellOutputMonitor::ShellOutputMonitor(std::string display, SnapshotObserver snapshotObserver,
                                       FailureObserver failureObserver, QObject *parent)
    : QObject(parent), display_(std::move(display)),
      snapshot_observer_(std::move(snapshotObserver)),
      failure_observer_(std::move(failureObserver)) {}

ShellOutputMonitor::~ShellOutputMonitor() { stop(); }

Result<void> ShellOutputMonitor::start() {
    auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (running_.exchange(true) || application == nullptr || !snapshot_observer_) {
        running_ = false;
        return Result<void>::failure(invalidConfiguration());
    }
    wake_descriptor_ = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_descriptor_ < 0) {
        running_ = false;
        return Result<void>::failure(monitorError());
    }
    for (auto *screen : QGuiApplication::screens()) {
        if (screen != nullptr) {
            observeScreen(*screen);
        }
    }
    connect(application, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        if (screen != nullptr) {
            observeScreen(*screen);
            requestRefresh();
        }
    });
    connect(application, &QGuiApplication::screenRemoved, this,
            [this](QScreen *) { requestRefresh(); });
    worker_ = std::jthread([this](std::stop_token token) { workerMain(token); });
    return Result<void>::success();
}

void ShellOutputMonitor::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }
    worker_.request_stop();
    requestRefresh();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (wake_descriptor_ >= 0) {
        (void)::close(wake_descriptor_);
        wake_descriptor_ = -1;
    }
}

Result<void> ShellOutputMonitor::configure(std::uint32_t panelHeightLogical,
                                           outputs::PanelPlacementPolicy policy,
                                           std::uint64_t settingsGeneration) {
    if (settingsGeneration == 0U || settingsGeneration <= settings_generation_ ||
        (policy.policy == outputs::PanelOutputPolicy::named && policy.outputNames.empty())) {
        return Result<void>::failure(invalidConfiguration());
    }
    auto candidate = registry_;
    if (!candidate) {
        auto created = outputs::OutputSurfaceRegistry::create(panelHeightLogical);
        if (!created) {
            return Result<void>::failure(created.error());
        }
        candidate = std::move(created).value();
    }
    if (topology_) {
        auto snapshot =
            candidate->reconfigure(panelHeightLogical, *topology_, policy, settingsGeneration);
        if (!snapshot) {
            return Result<void>::failure(snapshot.error());
        }
        if (snapshot_observer_) {
            auto applied = snapshot_observer_(snapshot.value());
            if (!applied) {
                return applied;
            }
        }
    }
    registry_ = std::move(candidate);
    panel_policy_ = std::move(policy);
    settings_generation_ = settingsGeneration;
    return Result<void>::success();
}

void ShellOutputMonitor::requestRefresh() noexcept {
    if (wake_descriptor_ < 0) {
        return;
    }
    const std::uint64_t wake = 1U;
    const auto written = ::write(wake_descriptor_, &wake, sizeof(wake));
    (void)written;
}

void ShellOutputMonitor::observeScreen(QScreen &screen) {
    connect(&screen, &QScreen::geometryChanged, this, [this](const QRect &) { requestRefresh(); });
    connect(&screen, &QScreen::logicalDotsPerInchChanged, this,
            [this](qreal) { requestRefresh(); });
    connect(&screen, &QScreen::physicalDotsPerInchChanged, this,
            [this](qreal) { requestRefresh(); });
}

void ShellOutputMonitor::workerMain(std::stop_token stopToken) noexcept {
    auto connection = x11::X11Connection::connect(display_);
    if (!connection) {
        publishFailure(connection.error(), true);
        return;
    }
    auto randr = x11::RandrTopologyProtocol::negotiate(connection.value());
    if (!randr) {
        publishFailure(randr.error(), true);
        return;
    }
    auto events = x11::RootEventStream::create(connection.value(), randr.value());
    if (!events) {
        publishFailure(events.error(), true);
        return;
    }

    bool refresh = true;
    while (!stopToken.stop_requested()) {
        if (refresh) {
            auto snapshot = randr.value().query(connection.value());
            if (!snapshot) {
                publishFailure(snapshot.error(), !connection.value().healthy());
                if (!connection.value().healthy()) {
                    return;
                }
            } else {
                publishTopology(std::move(snapshot).value());
            }
            refresh = false;
        }

        std::array<pollfd, 2> descriptors{{
            {connection.value().eventFileDescriptor(), POLLIN, 0},
            {wake_descriptor_, POLLIN, 0},
        }};
        int polled = -1;
        do {
            polled = ::poll(descriptors.data(), descriptors.size(), -1);
        } while (polled < 0 && errno == EINTR && !stopToken.stop_requested());
        if (stopToken.stop_requested()) {
            return;
        }
        if (polled < 0) {
            publishFailure(monitorError(), true);
            return;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            std::uint64_t value = 0U;
            while (::read(wake_descriptor_, &value, sizeof(value)) > 0) {
            }
            refresh = true;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            publishFailure(monitorError(), true);
            return;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            auto batch = events.value().drain();
            if (!batch) {
                publishFailure(batch.error(), !connection.value().healthy());
                if (!connection.value().healthy()) {
                    return;
                }
            } else {
                for (const auto &event : batch.value().events) {
                    refresh = refresh || needsRefresh(event);
                }
                if (batch.value().examinationLimitReached) {
                    refresh = true;
                }
            }
        }
    }
}

void ShellOutputMonitor::publishTopology(x11::RandrTopologySnapshot snapshot) noexcept {
    QPointer<ShellOutputMonitor> guard{this};
    QMetaObject::invokeMethod(
        this,
        [guard, snapshot = std::move(snapshot)]() mutable {
            if (guard) {
                guard->applyTopology(std::move(snapshot));
            }
        },
        Qt::QueuedConnection);
}

void ShellOutputMonitor::publishFailure(foundation::Error error, bool fatal) noexcept {
    QPointer<ShellOutputMonitor> guard{this};
    QMetaObject::invokeMethod(
        this,
        [guard, error = std::move(error), fatal]() {
            if (guard && guard->failure_observer_) {
                guard->failure_observer_(error, fatal);
            }
        },
        Qt::QueuedConnection);
}

void ShellOutputMonitor::applyTopology(x11::RandrTopologySnapshot snapshot) {
    auto root = x11::RootGeometry::fromScreenInfo(snapshot.coreScreen);
    if (!root) {
        publishFailure(root.error(), false);
        return;
    }
    auto topology = x11::buildOutputTopology(
        {snapshot.randrAvailable(), root.value(), snapshot.resourceOutputCount,
         snapshot.resourceCrtcCount, snapshot.resourceModeCount, std::move(snapshot.activeOutputs),
         snapshot.primary});
    if (!topology) {
        publishFailure(topology.error(), false);
        return;
    }
    std::vector<outputs::QtScreenObservation> screens;
    screens.reserve(static_cast<std::size_t>(QGuiApplication::screens().size()));
    for (const auto *screen : QGuiApplication::screens()) {
        if (screen == nullptr) {
            continue;
        }
        const auto rectangle = screen->geometry();
        if (rectangle.width() <= 0 || rectangle.height() <= 0) {
            publishFailure(invalidConfiguration(), false);
            return;
        }
        screens.push_back(
            {screen->name().toStdString(),
             {rectangle.x(), rectangle.y(), static_cast<std::uint32_t>(rectangle.width()),
              static_cast<std::uint32_t>(rectangle.height())},
             screen->devicePixelRatio()});
    }
    auto applied = observeTopology(topology.value(), screens);
    if (!applied) {
        publishFailure(applied.error(), false);
    }
}

Result<void>
ShellOutputMonitor::observeTopology(const x11::OutputTopologySnapshot &topology,
                                    std::span<const outputs::QtScreenObservation> screens) {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure(invalidConfiguration());
    }
    const auto candidateGeneration = generation_ + 1U;
    auto reconciled = outputs::reconcileScreens(candidateGeneration, topology, screens);
    if (!reconciled) {
        return Result<void>::failure(reconciled.error());
    }
    if (!registry_) {
        topology_ = reconciled.value();
        generation_ = candidateGeneration;
        return Result<void>::success();
    }
    auto candidateRegistry = *registry_;
    auto publication =
        candidateRegistry.observe(reconciled.value(), panel_policy_, settings_generation_);
    if (!publication) {
        return Result<void>::failure(publication.error());
    }
    auto applied = snapshot_observer_(publication.value());
    if (!applied) {
        return applied;
    }
    topology_ = std::move(reconciled).value();
    registry_ = std::move(candidateRegistry);
    generation_ = candidateGeneration;
    return Result<void>::success();
}

} // namespace prismdrake::shell::runtime
