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
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
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

class X11OutputMonitorOperations final : public ShellOutputMonitorOperations {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ShellOutputMonitorOperations>>
    create(std::string display) {
        auto connection = x11::X11Connection::connect(display);
        if (!connection) {
            return Result<std::unique_ptr<ShellOutputMonitorOperations>>::failure(
                connection.error());
        }
        return Result<std::unique_ptr<ShellOutputMonitorOperations>>::success(
            std::unique_ptr<ShellOutputMonitorOperations>(
                new X11OutputMonitorOperations(std::move(connection).value())));
    }

    Result<void> initialize() override {
        auto randr = x11::RandrTopologyProtocol::negotiate(connection_);
        if (!randr) {
            return Result<void>::failure(randr.error());
        }
        auto events = x11::RootEventStream::create(connection_, randr.value());
        if (!events) {
            return Result<void>::failure(events.error());
        }
        randr_.emplace(std::move(randr).value());
        events_.emplace(std::move(events).value());
        return Result<void>::success();
    }

    Result<x11::RandrTopologySnapshot> query() override {
        if (!randr_) {
            return Result<x11::RandrTopologySnapshot>::failure(monitorError());
        }
        return randr_->query(connection_);
    }

    [[nodiscard]] int eventFileDescriptor() const noexcept override {
        return connection_.eventFileDescriptor();
    }

    Result<ShellOutputMonitorEventDrain> drainEvents() override {
        if (!events_) {
            return Result<ShellOutputMonitorEventDrain>::failure(monitorError());
        }
        auto batch = events_->drain();
        if (!batch) {
            return Result<ShellOutputMonitorEventDrain>::failure(batch.error());
        }
        bool refresh = false;
        for (const auto &event : batch.value().events) {
            refresh = refresh || needsRefresh(event);
        }
        return Result<ShellOutputMonitorEventDrain>::success(
            {refresh, batch.value().examinationLimitReached});
    }

    void cancelWaits() noexcept override {
        const int descriptor = connection_.eventFileDescriptor();
        if (descriptor >= 0) {
            (void)::shutdown(descriptor, SHUT_RDWR);
        }
    }

  private:
    explicit X11OutputMonitorOperations(x11::X11Connection connection)
        : connection_(std::move(connection)) {}

    x11::X11Connection connection_;
    std::optional<x11::RandrTopologyProtocol> randr_;
    std::optional<x11::RootEventStream> events_;
};

class OperationsGuard final {
  public:
    OperationsGuard(std::atomic<ShellOutputMonitorOperations *> &published, std::mutex &mutex,
                    ShellOutputMonitorOperations *operations)
        : published_(published), mutex_(mutex) {
        std::scoped_lock lock{mutex_};
        published_.store(operations, std::memory_order_release);
    }
    ~OperationsGuard() {
        std::scoped_lock lock{mutex_};
        published_.store(nullptr, std::memory_order_release);
    }

  private:
    std::atomic<ShellOutputMonitorOperations *> &published_;
    std::mutex &mutex_;
};

} // namespace

struct ShellOutputMonitor::PublicationState final {
    struct Failure final {
        foundation::Error error;
        bool fatal;
        std::uint64_t topologyEpoch;
    };

    std::mutex mutex;
    std::optional<x11::RandrTopologySnapshot> topology;
    std::optional<Failure> failure;
    bool queued{false};
    bool stopping{false};
    std::uint64_t topologyEpoch{0U};
};

ShellOutputMonitor::ShellOutputMonitor(std::string display, SnapshotObserver snapshotObserver,
                                       FailureObserver failureObserver, QObject *parent)
    : ShellOutputMonitor(
          [display = std::move(display)] { return X11OutputMonitorOperations::create(display); },
          std::move(snapshotObserver), std::move(failureObserver), parent) {
    observe_qt_screens_ = true;
}

ShellOutputMonitor::ShellOutputMonitor(OperationsFactory operationsFactory,
                                       SnapshotObserver snapshotObserver,
                                       FailureObserver failureObserver, QObject *parent)
    : QObject(parent), operations_factory_(std::move(operationsFactory)),
      observe_qt_screens_(false), snapshot_observer_(std::move(snapshotObserver)),
      failure_observer_(std::move(failureObserver)) {}

ShellOutputMonitor::~ShellOutputMonitor() { stop(); }

Result<void> ShellOutputMonitor::start() {
    auto *coreApplication = QCoreApplication::instance();
    auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (coreApplication == nullptr || !operations_factory_ || !snapshot_observer_ ||
        (observe_qt_screens_ && application == nullptr)) {
        return Result<void>::failure(invalidConfiguration());
    }
    bool stopped = false;
    if (!running_.compare_exchange_strong(stopped, true)) {
        return Result<void>::failure(invalidConfiguration());
    }
    wake_descriptor_ = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_descriptor_ < 0) {
        running_ = false;
        return Result<void>::failure(monitorError());
    }
    try {
        std::scoped_lock lock{publication_state_mutex_};
        publication_state_ = std::make_shared<PublicationState>();
    } catch (const std::exception &) {
        (void)::close(wake_descriptor_);
        wake_descriptor_ = -1;
        running_ = false;
        return Result<void>::failure(monitorError());
    }
    if (application != nullptr) {
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
    }
    try {
        worker_ = std::jthread([this](std::stop_token token) { workerMain(token); });
    } catch (const std::exception &) {
        disconnectScreenObservers();
        if (auto state = publicationState()) {
            std::scoped_lock lock{state->mutex};
            state->stopping = true;
        }
        (void)::close(wake_descriptor_);
        wake_descriptor_ = -1;
        running_ = false;
        return Result<void>::failure(monitorError());
    }
    return Result<void>::success();
}

void ShellOutputMonitor::stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }
    disconnectScreenObservers();
    if (auto state = publicationState()) {
        std::scoped_lock lock{state->mutex};
        state->stopping = true;
        state->topology.reset();
        state->failure.reset();
    }
    worker_.request_stop();
    requestRefresh();
    {
        std::scoped_lock lock{operations_mutex_};
        if (auto *operations = operations_.load(std::memory_order_acquire); operations != nullptr) {
            operations->cancelWaits();
        }
    }
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

void ShellOutputMonitor::beginSettingsOwnerEpoch() noexcept {
    settings_generation_ = 0U;
    if (registry_) {
        registry_->beginSettingsOwnerEpoch();
    }
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

void ShellOutputMonitor::disconnectScreenObservers() noexcept {
    auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (application == nullptr) {
        return;
    }
    QObject::disconnect(application, nullptr, this, nullptr);
    for (auto *screen : QGuiApplication::screens()) {
        if (screen != nullptr) {
            QObject::disconnect(screen, nullptr, this, nullptr);
        }
    }
}

void ShellOutputMonitor::workerMain(std::stop_token stopToken) noexcept {
    auto operations = operations_factory_();
    if (!operations) {
        if (!stopToken.stop_requested()) {
            publishFailure(operations.error(), true);
        }
        return;
    }
    OperationsGuard operationsGuard{operations_, operations_mutex_, operations.value().get()};
    if (stopToken.stop_requested()) {
        return;
    }
    auto initialized = operations.value()->initialize();
    if (stopToken.stop_requested()) {
        return;
    }
    if (!initialized) {
        publishFailure(initialized.error(), true);
        return;
    }

    bool refresh = true;
    bool drainFollowUp = true;
    while (!stopToken.stop_requested()) {
        if (refresh) {
            auto snapshot = operations.value()->query();
            if (stopToken.stop_requested()) {
                return;
            }
            drainFollowUp = true;
            if (!snapshot) {
                const bool fatal = snapshot.error().code == ErrorCode::io_error;
                publishFailure(snapshot.error(), fatal);
                if (fatal) {
                    return;
                }
            } else {
                publishTopology(std::move(snapshot).value());
            }
            refresh = false;
        }

        if (drainFollowUp) {
            auto drained = operations.value()->drainEvents();
            if (stopToken.stop_requested()) {
                return;
            }
            if (!drained) {
                const bool fatal = drained.error().code == ErrorCode::io_error;
                publishFailure(drained.error(), fatal);
                if (fatal) {
                    return;
                }
                drainFollowUp = false;
            } else {
                refresh = refresh || drained.value().refreshRequired;
                drainFollowUp = drained.value().followUpRequired;
            }
            continue;
        }

        std::array<pollfd, 2> descriptors{{
            {operations.value()->eventFileDescriptor(), POLLIN, 0},
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
            auto drained = operations.value()->drainEvents();
            if (!drained) {
                const bool fatal = drained.error().code == ErrorCode::io_error;
                publishFailure(drained.error(), fatal);
                if (fatal) {
                    return;
                }
            } else {
                refresh = refresh || drained.value().refreshRequired;
                drainFollowUp = drained.value().followUpRequired;
            }
        }
    }
}

void ShellOutputMonitor::publishTopology(x11::RandrTopologySnapshot snapshot) noexcept {
    auto state = publicationState();
    if (!state) {
        return;
    }
    bool queue = false;
    {
        std::scoped_lock lock{state->mutex};
        if (state->stopping) {
            return;
        }
        ++state->topologyEpoch;
        state->topology = std::move(snapshot);
        if (state->failure && !state->failure->fatal) {
            state->failure.reset();
        }
        if (!state->queued) {
            state->queued = true;
            queue = true;
        }
    }
    if (queue) {
        queuePublication(state);
    }
}

void ShellOutputMonitor::publishFailure(foundation::Error error, bool fatal) noexcept {
    auto state = publicationState();
    if (!state) {
        return;
    }
    bool queue = false;
    {
        std::scoped_lock lock{state->mutex};
        if (state->stopping) {
            return;
        }
        if (!state->failure || fatal || !state->failure->fatal) {
            state->failure =
                PublicationState::Failure{std::move(error), fatal, state->topologyEpoch};
        }
        if (fatal) {
            state->topology.reset();
        }
        if (!state->queued) {
            state->queued = true;
            queue = true;
        }
    }
    if (queue) {
        queuePublication(state);
    }
}

std::shared_ptr<ShellOutputMonitor::PublicationState>
ShellOutputMonitor::publicationState() const noexcept {
    std::scoped_lock lock{publication_state_mutex_};
    return publication_state_;
}

void ShellOutputMonitor::queuePublication(const std::shared_ptr<PublicationState> &state) noexcept {
    QPointer<ShellOutputMonitor> guard{this};
    const bool accepted = QMetaObject::invokeMethod(
        this,
        [guard, state] {
            std::optional<x11::RandrTopologySnapshot> topology;
            std::optional<PublicationState::Failure> failure;
            {
                std::scoped_lock lock{state->mutex};
                if (state->stopping) {
                    state->queued = false;
                    return;
                }
                if (state->failure && state->failure->fatal) {
                    failure = std::move(state->failure);
                    state->failure.reset();
                    state->topology.reset();
                } else if (state->topology) {
                    topology = std::move(state->topology);
                    state->topology.reset();
                } else if (state->failure) {
                    failure = std::move(state->failure);
                    state->failure.reset();
                }
            }
            if (guard && topology) {
                guard->applyTopology(std::move(*topology));
            } else if (guard && failure && guard->failure_observer_) {
                bool superseded = false;
                {
                    std::scoped_lock lock{state->mutex};
                    superseded = !failure->fatal && failure->topologyEpoch != state->topologyEpoch;
                }
                if (!superseded) {
                    guard->failure_observer_(failure->error, failure->fatal);
                }
            }
            bool queueAgain = false;
            {
                std::scoped_lock lock{state->mutex};
                if (state->stopping) {
                    state->topology.reset();
                    state->failure.reset();
                    state->queued = false;
                    return;
                }
                if (state->topology || state->failure) {
                    queueAgain = true;
                } else {
                    state->queued = false;
                }
            }
            if (queueAgain && guard) {
                guard->queuePublication(state);
            }
        },
        Qt::QueuedConnection);
    if (!accepted) {
        std::scoped_lock lock{state->mutex};
        state->queued = false;
    }
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
    const auto qtScreens = observe_qt_screens_ ? QGuiApplication::screens() : QList<QScreen *>{};
    screens.reserve(static_cast<std::size_t>(qtScreens.size()));
    for (const auto *screen : qtScreens) {
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
