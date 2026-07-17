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
    explicit DefaultLaunchBackend(std::unique_ptr<LauncherProcessExecutor> executor)
        : executor_(std::move(executor)) {}

    Result<void> launch(const prismdrake::launcher::DiscoveredDesktopEntry &discovered,
                        const prismdrake::launcher::ProcessLaunchContext &context,
                        const foundation::CancellationToken &cancellation) override {
        auto provenance = prismdrake::launcher::validateDiscoveredDesktopFileLocation(
            discovered.id, discovered.location);
        if (!provenance) {
            return provenance;
        }
        prismdrake::launcher::DesktopExecExpansionContext expansion;
        expansion.desktopFileLocation = discovered.location.absolutePath().native();
        auto invocations = prismdrake::launcher::expandDesktopExec(discovered.entry, expansion);
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
                prismdrake::launcher::makeProcessLaunchPlan(discovered.entry, invocation, context);
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

            auto result = launch_backend_->launch(job.entry, owner_->options_.launchContext, token);
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
makeDefaultLauncherLaunchBackend(std::unique_ptr<LauncherProcessExecutor> executor) {
    if (!executor) {
        executor = std::make_unique<DetachedProcessExecutor>();
    }
    return std::make_unique<DefaultLaunchBackend>(std::move(executor));
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
    return Result<std::unique_ptr<LauncherController>>::success(
        std::unique_ptr<LauncherController>(new LauncherController(
            std::move(options), std::move(indexBackend), std::move(launchBackend))));
}

LauncherController::LauncherController(LauncherControllerOptions options,
                                       std::unique_ptr<LauncherIndexBackend> indexBackend,
                                       std::unique_ptr<LauncherLaunchBackend> launchBackend)
    : options_(std::move(options)), presentation_(this),
      query_(std::move(prismdrake::launcher::parseApplicationSearchQuery({})).value()),
      implementation_(
          std::make_unique<Impl>(*this, std::move(indexBackend), std::move(launchBackend))) {
    connect(&presentation_, &LauncherPresentationModel::launchRequested, this,
            &LauncherController::handleLaunchIntent);
}

LauncherController::~LauncherController() { implementation_->stop(); }

Result<void> LauncherController::refresh() {
    if (QThread::currentThread() != thread()) {
        return ownerThreadError();
    }
    if (catalog_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure({ErrorCode::too_large, "launcher catalog generation exhausted",
                                      "restart the shell launcher controller"});
    }
    ++catalog_generation_;
    catalog_.reset();
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
    if (!result || !presentation_.applySnapshot(catalog_, result.value())) {
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
            LaunchJob{catalog_->discovery->entries[match->discoveryEntryIndex]})) {
        emit launchRejected();
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
