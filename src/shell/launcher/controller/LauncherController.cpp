#include "LauncherController.hpp"

#include "DesktopEntryDiscovery.hpp"
#include "DesktopExec.hpp"
#include "DetachedApplication.hpp"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace prismdrake::shell::launcher::controller {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using Catalog = std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot>;
using Search = std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot>;

[[nodiscard]] Catalog makeRefreshErrorCatalog(std::uint64_t generation) noexcept {
    try {
        auto discovery =
            std::make_shared<const prismdrake::launcher::DesktopEntryDiscoverySnapshot>(
                prismdrake::launcher::DesktopEntryDiscoverySnapshot{
                    {}, {}, {}, 0U, true, false, false});
        return std::make_shared<const prismdrake::launcher::ApplicationCatalogSnapshot>(
            prismdrake::launcher::ApplicationCatalogSnapshot{
                generation, std::move(discovery), {}, {}, 0U, 0U, true});
    } catch (const std::bad_alloc &) {
        return {};
    }
}

[[nodiscard]] Result<void> ownerThreadError() {
    return Result<void>::failure({ErrorCode::cancelled,
                                  "launcher controller was called from a non-owner thread",
                                  "queue launcher requests to the controller QObject thread"});
}

class DefaultIndexBackend final : public LauncherIndexBackend {
  public:
    Result<Catalog> refresh(const LauncherControllerOptions &options,
                            std::uint64_t catalogGeneration,
                            const foundation::CancellationToken &cancellation) override {
        auto discovery = prismdrake::launcher::createDesktopEntryDiscovery(
            options.applicationPaths, options.parseContext, options.currentDesktop,
            options.discoveryLimits);
        if (!discovery) {
            return Result<Catalog>::failure(discovery.error());
        }
        std::shared_ptr<const prismdrake::launcher::DesktopEntryDiscoverySnapshot> snapshot;
        while (!snapshot || !snapshot->complete) {
            auto batch = discovery.value().pull(
                prismdrake::launcher::maximumDesktopDiscoveryNodesPerRoot, cancellation);
            if (!batch) {
                return Result<Catalog>::failure(batch.error());
            }
            auto discoveryBatch = std::move(batch).value();
            if (discoveryBatch.cancellationObserved || cancellation.isCancellationRequested()) {
                return Result<Catalog>::failure(
                    {ErrorCode::cancelled, "application discovery was cancelled",
                     "discard the stale discovery and run the replacement refresh"});
            }
            snapshot = std::move(discoveryBatch.snapshot);
        }

        auto operation = prismdrake::launcher::createApplicationCatalog(
            std::move(snapshot), options.executableLookup, catalogGeneration);
        if (!operation) {
            return Result<Catalog>::failure(operation.error());
        }
        Catalog catalog;
        while (!catalog || !catalog->complete) {
            auto batch = operation.value().pull(
                prismdrake::launcher::maximumApplicationCatalogWorkUnits, cancellation);
            if (!batch) {
                return Result<Catalog>::failure(batch.error());
            }
            catalog = std::move(batch).value().snapshot;
        }
        return Result<Catalog>::success(std::move(catalog));
    }

    Result<Search> search(Catalog catalog, std::uint64_t requestGeneration,
                          const prismdrake::launcher::ApplicationSearchQuery &query,
                          const foundation::CancellationToken &cancellation) override {
        auto operation = prismdrake::launcher::createApplicationSearch(
            std::move(catalog), requestGeneration, query,
            prismdrake::launcher::maximumApplicationSearchResults);
        if (!operation) {
            return Result<Search>::failure(operation.error());
        }
        Search search;
        do {
            auto advanced = operation.value().advance(
                prismdrake::launcher::maximumApplicationSearchWorkUnits, cancellation);
            if (!advanced) {
                return Result<Search>::failure(advanced.error());
            }
            search = std::move(advanced).value();
        } while (search->state == prismdrake::launcher::ApplicationSearchViewState::loading);
        return Result<Search>::success(std::move(search));
    }
};

class DetachedProcessExecutor final : public LauncherProcessExecutor {
  public:
    Result<void> execute(const prismdrake::launcher::ProcessLaunchPlan &plan,
                         const foundation::CancellationToken &cancellation) override {
        return prismdrake::launcher::launchDetachedApplication(plan, cancellation);
    }
};

class DefaultLaunchBackend final : public LauncherLaunchBackend {
  public:
    DefaultLaunchBackend(std::unique_ptr<LauncherProcessExecutor> executor,
                         std::unique_ptr<LauncherApplicationActivator> activator)
        : executor_(std::move(executor)), activator_(std::move(activator)) {}

    Result<void> launch(const prismdrake::launcher::DiscoveredDesktopEntry &discovered,
                        std::optional<std::string> desktopActionId,
                        const prismdrake::launcher::ProcessLaunchContext &context,
                        const foundation::CancellationToken &cancellation) override {
        auto provenance = prismdrake::launcher::validateDiscoveredDesktopFileLocation(
            discovered.id, discovered.location);
        if (!provenance) {
            return provenance;
        }
        std::optional<std::string> selectedExec = discovered.entry.exec;
        if (desktopActionId) {
            const auto action =
                std::ranges::find_if(discovered.entry.actions, [&](const auto &candidate) {
                    return candidate.identifier == *desktopActionId;
                });
            if (action == discovered.entry.actions.end()) {
                return Result<void>::failure({ErrorCode::validation_error,
                                              "The selected desktop action is unavailable.",
                                              "Choose a current bounded desktop action."});
            }
            selectedExec = action->exec;
        }
        if (discovered.entry.dbusActivatable) {
            auto activated = activator_->activate(
                discovered.id,
                desktopActionId ? std::optional<std::string_view>{*desktopActionId} : std::nullopt,
                cancellation);
            if (activated) {
                return activated;
            }
            // The Desktop Entry contract permits Exec fallback only when the selected launch
            // target provides an Exec value. Never manufacture a command for activation-only
            // entries.
            if (!selectedExec) {
                return activated;
            }
        }

        auto directEntry = discovered.entry;
        directEntry.dbusActivatable = false;
        directEntry.exec = std::move(selectedExec);
        prismdrake::launcher::DesktopExecExpansionContext expansion;
        expansion.desktopFileLocation = discovered.location.absolutePath().native();
        auto invocations = prismdrake::launcher::expandDesktopExec(directEntry, expansion);
        if (!invocations) {
            return Result<void>::failure(invocations.error());
        }
        for (const auto &invocation : invocations.value()) {
            if (cancellation.isCancellationRequested()) {
                return Result<void>::failure({ErrorCode::cancelled,
                                              "application launch was cancelled",
                                              "discard the stale launch request"});
            }
            auto plan =
                prismdrake::launcher::makeProcessLaunchPlan(directEntry, invocation, context);
            if (!plan) {
                return Result<void>::failure(plan.error());
            }
            auto launched = executor_->execute(plan.value(), cancellation);
            if (!launched) {
                return launched;
            }
        }
        return Result<void>::success();
    }

  private:
    std::unique_ptr<LauncherProcessExecutor> executor_;
    std::unique_ptr<LauncherApplicationActivator> activator_;
};

struct RefreshJob final {
    std::uint64_t generation;
};
struct SearchJob final {
    Catalog catalog;
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
    prismdrake::launcher::ApplicationSearchQuery query;
};
using IndexJob = std::variant<RefreshJob, SearchJob>;

struct LaunchJob final {
    prismdrake::launcher::DiscoveredDesktopEntry entry;
    std::optional<std::string> desktopActionId;
};

} // namespace

class LauncherController::Impl final {
  public:
    Impl(LauncherController &owner, std::unique_ptr<LauncherIndexBackend> indexBackend,
         std::unique_ptr<LauncherLaunchBackend> launchBackend)
        : owner_(&owner), index_backend_(std::move(indexBackend)),
          launch_backend_(std::move(launchBackend)),
          index_thread_([this](std::stop_token) { runIndex(); }),
          launch_thread_([this](std::stop_token) { runLaunch(); }) {}

    ~Impl() { stop(); }

    void submitIndex(IndexJob job) {
        std::lock_guard lock(index_mutex_);
        if (index_cancellation_) {
            static_cast<void>(index_cancellation_->requestCancellation());
        }
        pending_index_ = std::move(job);
        index_condition_.notify_one();
    }

    [[nodiscard]] bool submitLaunch(LaunchJob job) {
        std::lock_guard lock(launch_mutex_);
        if (stopping_ || pending_launch_ || launch_processing_) {
            return false;
        }
        pending_launch_ = std::move(job);
        launch_condition_.notify_one();
        return true;
    }

    void stop() noexcept {
        {
            std::scoped_lock lock(index_mutex_, launch_mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            pending_index_.reset();
            pending_launch_.reset();
            if (index_cancellation_) {
                static_cast<void>(index_cancellation_->requestCancellation());
            }
            if (launch_cancellation_) {
                static_cast<void>(launch_cancellation_->requestCancellation());
            }
        }
        index_condition_.notify_one();
        launch_condition_.notify_one();
        if (index_thread_.joinable()) {
            index_thread_.join();
        }
        if (launch_thread_.joinable()) {
            launch_thread_.join();
        }
    }

  private:
    void runIndex() {
        while (true) {
            std::unique_lock lock(index_mutex_);
            index_condition_.wait(lock, [this] { return stopping_ || pending_index_.has_value(); });
            if (stopping_) {
                return;
            }
            auto job = std::move(pending_index_).value();
            pending_index_.reset();
            index_cancellation_ = std::make_shared<foundation::CancellationSource>();
            const auto token = index_cancellation_->token();
            lock.unlock();

            std::visit(
                [this, &token](auto &&operation) {
                    using Operation = std::remove_cvref_t<decltype(operation)>;
                    if constexpr (std::is_same_v<Operation, RefreshJob>) {
                        auto result =
                            index_backend_->refresh(owner_->options_, operation.generation, token);
                        QMetaObject::invokeMethod(
                            owner_,
                            [owner = owner_, generation = operation.generation,
                             result = std::move(result)]() mutable {
                                owner->handleRefreshCompletion(generation, std::move(result));
                            },
                            Qt::QueuedConnection);
                    } else {
                        auto result = index_backend_->search(
                            operation.catalog, operation.requestGeneration, operation.query, token);
                        QMetaObject::invokeMethod(
                            owner_,
                            [owner = owner_, catalogGeneration = operation.catalogGeneration,
                             requestGeneration = operation.requestGeneration,
                             result = std::move(result)]() mutable {
                                owner->handleSearchCompletion(catalogGeneration, requestGeneration,
                                                              std::move(result));
                            },
                            Qt::QueuedConnection);
                    }
                },
                std::move(job));

            lock.lock();
            index_cancellation_.reset();
        }
    }

    void runLaunch() {
        while (true) {
            std::unique_lock lock(launch_mutex_);
            launch_condition_.wait(lock,
                                   [this] { return stopping_ || pending_launch_.has_value(); });
            if (stopping_) {
                return;
            }
            auto job = std::move(pending_launch_).value();
            pending_launch_.reset();
            launch_processing_ = true;
            launch_cancellation_ = std::make_shared<foundation::CancellationSource>();
            const auto token = launch_cancellation_->token();
            lock.unlock();

            auto result = launch_backend_->launch(job.entry, std::move(job.desktopActionId),
                                                  owner_->options_.launchContext, token);
            QMetaObject::invokeMethod(
                owner_,
                [owner = owner_, result = std::move(result)]() mutable {
                    owner->handleLaunchCompletion(std::move(result));
                },
                Qt::QueuedConnection);

            lock.lock();
            launch_processing_ = false;
            launch_cancellation_.reset();
        }
    }

    LauncherController *owner_;
    std::unique_ptr<LauncherIndexBackend> index_backend_;
    std::unique_ptr<LauncherLaunchBackend> launch_backend_;
    std::mutex index_mutex_;
    std::mutex launch_mutex_;
    std::condition_variable index_condition_;
    std::condition_variable launch_condition_;
    std::optional<IndexJob> pending_index_;
    std::optional<LaunchJob> pending_launch_;
    std::shared_ptr<foundation::CancellationSource> index_cancellation_;
    std::shared_ptr<foundation::CancellationSource> launch_cancellation_;
    bool launch_processing_{false};
    bool stopping_{false};
    std::jthread index_thread_;
    std::jthread launch_thread_;
};

std::unique_ptr<LauncherIndexBackend> makeDefaultLauncherIndexBackend() {
    return std::make_unique<DefaultIndexBackend>();
}

std::unique_ptr<LauncherLaunchBackend>
makeDefaultLauncherLaunchBackend(std::unique_ptr<LauncherProcessExecutor> executor,
                                 std::unique_ptr<LauncherApplicationActivator> activator) {
    if (!executor) {
        executor = std::make_unique<DetachedProcessExecutor>();
    }
    if (!activator) {
        activator = makeDefaultLauncherApplicationActivator();
    }
    return std::make_unique<DefaultLaunchBackend>(std::move(executor), std::move(activator));
}

Result<std::unique_ptr<LauncherController>>
LauncherController::create(LauncherControllerOptions options,
                           std::unique_ptr<LauncherIndexBackend> indexBackend,
                           std::unique_ptr<LauncherLaunchBackend> launchBackend) {
    auto validation = prismdrake::launcher::createDesktopEntryDiscovery(
        options.applicationPaths, options.parseContext, options.currentDesktop,
        options.discoveryLimits);
    if (!validation) {
        return Result<std::unique_ptr<LauncherController>>::failure(validation.error());
    }
    if (!indexBackend) {
        indexBackend = makeDefaultLauncherIndexBackend();
    }
    if (!launchBackend) {
        launchBackend = makeDefaultLauncherLaunchBackend();
    }
    std::unique_ptr<ApplicationRootMonitor> monitor;
    if (options.automaticMonitoring) {
        auto createdMonitor = ApplicationRootMonitor::create(options.applicationPaths);
        if (!createdMonitor) {
            return Result<std::unique_ptr<LauncherController>>::failure(createdMonitor.error());
        }
        monitor = std::move(createdMonitor).value();
    }
    return Result<std::unique_ptr<LauncherController>>::success(std::unique_ptr<LauncherController>(
        new LauncherController(std::move(options), std::move(indexBackend),
                               std::move(launchBackend), std::move(monitor))));
}

LauncherController::LauncherController(LauncherControllerOptions options,
                                       std::unique_ptr<LauncherIndexBackend> indexBackend,
                                       std::unique_ptr<LauncherLaunchBackend> launchBackend,
                                       std::unique_ptr<ApplicationRootMonitor> monitor)
    : options_(std::move(options)), presentation_(this),
      query_(std::move(prismdrake::launcher::parseApplicationSearchQuery({})).value()),
      implementation_(
          std::make_unique<Impl>(*this, std::move(indexBackend), std::move(launchBackend))),
      monitor_(std::move(monitor)) {
    connect(&presentation_, &LauncherPresentationModel::launchRequested, this,
            &LauncherController::handleLaunchIntent);
    connect(&presentation_, &LauncherPresentationModel::actionRequested, this,
            &LauncherController::handleActionIntent);
    connect(&presentation_, &LauncherPresentationModel::pinRequested, this,
            &LauncherController::handlePinIntent);
    connect(&presentation_, &LauncherPresentationModel::destinationRequested, this,
            &LauncherController::handleDestinationIntent);
    connect(&presentation_, &LauncherPresentationModel::iconRequested, this,
            &LauncherController::handleIconIntent);
    connect(&presentation_, &LauncherPresentationModel::refreshRequested, this,
            [this] { static_cast<void>(refresh()); });
    if (monitor_) {
        monitor_->setParent(this);
        connect(monitor_.get(), &ApplicationRootMonitor::refreshRequested, this,
                [this] { static_cast<void>(beginRefresh()); });
        connect(monitor_.get(), &ApplicationRootMonitor::degradedChanged, this,
                &LauncherController::applicationMonitorDegraded);
    }
}

LauncherController::~LauncherController() { implementation_->stop(); }

Result<void> LauncherController::refresh() {
    if (QThread::currentThread() != thread()) {
        return ownerThreadError();
    }
    if (monitor_) {
        monitor_->requestRecovery();
        if (monitor_->degraded()) {
            emit applicationMonitorDegraded(true);
        }
    }
    return beginRefresh();
}

Result<void> LauncherController::beginRefresh() {
    if (catalog_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure({ErrorCode::too_large, "launcher catalog generation exhausted",
                                      "restart the shell launcher controller"});
    }
    ++catalog_generation_;
    catalog_.reset();
    pending_icons_.clear();
    queued_icon_requests_.clear();
    implementation_->submitIndex(RefreshJob{catalog_generation_});
    return Result<void>::success();
}

Result<void> LauncherController::setSearchQuery(const QString &query) {
    if (QThread::currentThread() != thread()) {
        return ownerThreadError();
    }
    const auto encoded = query.toUtf8();
    auto parsed = prismdrake::launcher::parseApplicationSearchQuery(
        std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())});
    if (!parsed) {
        return Result<void>::failure(parsed.error());
    }
    if (request_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure({ErrorCode::too_large, "launcher request generation exhausted",
                                      "restart the shell launcher controller"});
    }
    query_ = std::move(parsed).value();
    ++request_generation_;
    if (catalog_) {
        startCurrentSearch();
    }
    return Result<void>::success();
}

Result<void>
LauncherController::applyPinSnapshot(std::uint64_t generation,
                                     const std::vector<prismdrake::launcher::DesktopFileId> &pins) {
    if (QThread::currentThread() != thread()) {
        return ownerThreadError();
    }
    return presentation_.applyPinSnapshot(generation, pins);
}

Result<void> LauncherController::setIconThemeGeneration(std::uint64_t generation) {
    if (QThread::currentThread() != thread()) {
        return ownerThreadError();
    }
    if (generation == 0U || generation < icon_theme_generation_) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The launcher icon theme generation is invalid.",
                                      "Publish a nonzero monotonic immutable theme generation."});
    }
    if (generation == icon_theme_generation_) {
        return Result<void>::success();
    }
    icon_theme_generation_ = generation;
    pending_icons_.clear();
    queued_icon_requests_.clear();
    presentation_.requestAllIcons();
    return Result<void>::success();
}

void LauncherController::startCurrentSearch() {
    implementation_->submitIndex(
        SearchJob{catalog_, catalog_generation_, request_generation_, query_});
}

void LauncherController::handleRefreshCompletion(std::uint64_t generation, Result<Catalog> result) {
    if (generation != catalog_generation_) {
        return;
    }
    if (!result) {
        auto error = prismdrake::launcher::makeApplicationSearchErrorSnapshot(catalog_generation_,
                                                                              request_generation_);
        auto errorCatalog = makeRefreshErrorCatalog(generation);
        if (error && errorCatalog) {
            static_cast<void>(
                presentation_.applySnapshot(std::move(errorCatalog), std::move(error).value()));
        }
        emit catalogRefreshFailed();
        return;
    }
    catalog_ = std::move(result).value();
    startCurrentSearch();
}

void LauncherController::handleSearchCompletion(std::uint64_t catalogGeneration,
                                                std::uint64_t requestGeneration,
                                                Result<Search> result) {
    if (catalogGeneration != catalog_generation_ || requestGeneration != request_generation_ ||
        !catalog_) {
        return;
    }
    if (!result || !presentation_.applySnapshot(catalog_, result.value(), &query_)) {
        auto error = prismdrake::launcher::makeApplicationSearchErrorSnapshot(catalog_generation_,
                                                                              request_generation_);
        if (error) {
            static_cast<void>(presentation_.applySnapshot(catalog_, error.value()));
        }
        emit searchFailed();
    }
}

void LauncherController::handleLaunchIntent(const ApplicationLaunchIntent &intent) {
    if (!catalog_ || !presentation_.currentSearch() ||
        intent.catalogGeneration != catalog_generation_ ||
        intent.requestGeneration != request_generation_) {
        emit launchRejected();
        return;
    }
    const auto match =
        std::ranges::find_if(presentation_.currentSearch()->results, [&](const auto &result) {
            return catalog_->discovery->entries[result.discoveryEntryIndex].id ==
                   intent.desktopFileId;
        });
    if (match == presentation_.currentSearch()->results.end() ||
        !implementation_->submitLaunch(
            LaunchJob{catalog_->discovery->entries[match->discoveryEntryIndex], std::nullopt})) {
        emit launchRejected();
    }
}

void LauncherController::handleActionIntent(const ApplicationActionIntent &intent) {
    if (!catalog_ || !presentation_.currentSearch() ||
        intent.catalogGeneration != catalog_generation_ ||
        intent.requestGeneration != request_generation_) {
        emit launchRejected();
        return;
    }
    const auto match =
        std::ranges::find_if(presentation_.currentSearch()->results, [&](const auto &result) {
            return catalog_->discovery->entries[result.discoveryEntryIndex].id ==
                   intent.desktopFileId;
        });
    if (match == presentation_.currentSearch()->results.end()) {
        emit launchRejected();
        return;
    }
    const auto &entry = catalog_->discovery->entries[match->discoveryEntryIndex];
    const bool actionExists = std::ranges::any_of(entry.entry.actions, [&](const auto &action) {
        return action.identifier == intent.actionId;
    });
    if (!actionExists || !implementation_->submitLaunch(LaunchJob{entry, intent.actionId})) {
        emit launchRejected();
    }
}

void LauncherController::handlePinIntent(const ApplicationPinIntent &intent) {
    const bool currentIdentity =
        catalog_ && presentation_.currentSearch() &&
        std::ranges::any_of(presentation_.currentSearch()->results, [&](const auto &result) {
            return catalog_->discovery->entries[result.discoveryEntryIndex].id ==
                   intent.desktopFileId;
        });
    if (!catalog_ || !presentation_.currentSearch() ||
        intent.catalogGeneration != catalog_generation_ ||
        intent.requestGeneration != request_generation_ || !currentIdentity) {
        emit launchRejected();
        return;
    }
    emit pinChangeRequested(intent);
}

void LauncherController::handleDestinationIntent(const ShellDestinationIntent &intent) {
    if (!presentation_.currentSearch() || intent.requestGeneration != request_generation_) {
        return;
    }
    emit shellDestinationRequested(intent);
}

void LauncherController::handleIconIntent(const LauncherIconIntent &intent) {
    constexpr std::size_t maximumQueuedIcons =
        prismdrake::launcher::maximumApplicationSearchResults;
    const auto requestKey = std::pair{intent.opaqueKey, icon_theme_generation_};
    if (!catalog_ || intent.catalogGeneration != catalog_generation_ ||
        intent.opaqueKey.isEmpty() || queued_icon_requests_.contains(requestKey) ||
        pending_icons_.size() + active_icon_requests_ >= maximumQueuedIcons) {
        return;
    }
    queued_icon_requests_.emplace(requestKey);
    pending_icons_.push_back(intent);
    pumpIconQueue();
}

void LauncherController::pumpIconQueue() {
    constexpr std::size_t maximumConcurrentIcons = 4U;
    while (active_icon_requests_ < maximumConcurrentIcons && !pending_icons_.empty()) {
        auto intent = std::move(pending_icons_.front());
        pending_icons_.pop_front();
        const auto themeGeneration = icon_theme_generation_;
        prismdrake::icons::IconRequest request{intent.desktopFileId, intent.iconField, 32, 1.0,
                                               themeGeneration};
        auto submitted = icon_resolver_.resolve(
            std::move(request), this,
            [this, opaqueKey = intent.opaqueKey, catalogGeneration = intent.catalogGeneration,
             themeGeneration](prismdrake::icons::IconResolution resolution) {
                queued_icon_requests_.erase(std::pair{opaqueKey, themeGeneration});
                if (active_icon_requests_ > 0U) {
                    --active_icon_requests_;
                }
                const bool currentRequest = catalogGeneration == catalog_generation_ &&
                                            themeGeneration == icon_theme_generation_;
                if (currentRequest && resolution.themeGeneration == themeGeneration) {
                    presentation_.applyResolvedIcon(opaqueKey, resolution.image);
                } else {
                    // A replacement publication may have filled the bounded queue while this
                    // stale request still occupied a concurrency slot. Re-offer current rows;
                    // the request set suppresses duplicates and fills only the newly free slot.
                    presentation_.requestAllIcons();
                }
                pumpIconQueue();
            });
        if (submitted) {
            ++active_icon_requests_;
        } else {
            queued_icon_requests_.erase(std::pair{intent.opaqueKey, themeGeneration});
        }
    }
}

void LauncherController::handleLaunchCompletion(Result<void> result) {
    if (result) {
        emit launchCompleted();
    } else {
        emit launchFailed();
    }
}

} // namespace prismdrake::shell::launcher::controller
