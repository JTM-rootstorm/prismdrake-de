#pragma once

#include "ApplicationPaths.hpp"
#include "ApplicationSearch.hpp"
#include "DesktopEntryParser.hpp"
#include "DesktopEntryVisibility.hpp"
#include "LauncherPresentationModel.hpp"
#include "ProcessLaunch.hpp"
#include "Result.hpp"

#include <QObject>
#include <QString>

#include <memory>

namespace prismdrake::shell::launcher::controller {

struct LauncherControllerOptions final {
    prismdrake::launcher::ApplicationPaths applicationPaths;
    prismdrake::launcher::DesktopEntryParseContext parseContext;
    prismdrake::launcher::CurrentDesktopContext currentDesktop;
    prismdrake::launcher::DesktopEntryDiscoveryLimits discoveryLimits;
    prismdrake::launcher::DesktopExecutableLookupContext executableLookup;
    prismdrake::launcher::ProcessLaunchContext launchContext;
};

class LauncherIndexBackend {
  public:
    virtual ~LauncherIndexBackend() = default;

    /// Performs blocking discovery/catalog work on the controller's index thread.
    /// Implementations must observe cancellation promptly and return immutable snapshots only.
    [[nodiscard]] virtual foundation::Result<
        std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot>>
    refresh(const LauncherControllerOptions &options, std::uint64_t catalogGeneration,
            const foundation::CancellationToken &cancellation) = 0;

    [[nodiscard]] virtual foundation::Result<
        std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot>>
    search(std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog,
           std::uint64_t requestGeneration,
           const prismdrake::launcher::ApplicationSearchQuery &query,
           const foundation::CancellationToken &cancellation) = 0;
};

class LauncherProcessExecutor {
  public:
    virtual ~LauncherProcessExecutor() = default;
    [[nodiscard]] virtual foundation::Result<void>
    execute(const prismdrake::launcher::ProcessLaunchPlan &plan,
            const foundation::CancellationToken &cancellation) = 0;
};

class LauncherLaunchBackend {
  public:
    virtual ~LauncherLaunchBackend() = default;
    /// Performs blocking expansion, planning, and execution on the dedicated launch thread.
    [[nodiscard]] virtual foundation::Result<void>
    launch(const prismdrake::launcher::DiscoveredDesktopEntry &entry,
           const prismdrake::launcher::ProcessLaunchContext &context,
           const foundation::CancellationToken &cancellation) = 0;
};

[[nodiscard]] std::unique_ptr<LauncherIndexBackend> makeDefaultLauncherIndexBackend();
[[nodiscard]] std::unique_ptr<LauncherLaunchBackend>
makeDefaultLauncherLaunchBackend(std::unique_ptr<LauncherProcessExecutor> executor = {});

/// Owner-thread coordinator for asynchronous discovery, search, and safe application launch.
class LauncherController final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static foundation::Result<std::unique_ptr<LauncherController>>
    create(LauncherControllerOptions options,
           std::unique_ptr<LauncherIndexBackend> indexBackend = {},
           std::unique_ptr<LauncherLaunchBackend> launchBackend = {});

    ~LauncherController() override;

    LauncherController(const LauncherController &) = delete;
    LauncherController &operator=(const LauncherController &) = delete;

    [[nodiscard]] LauncherPresentationModel *presentationModel() noexcept { return &presentation_; }
    [[nodiscard]] foundation::Result<void> refresh();
    [[nodiscard]] foundation::Result<void> setSearchQuery(const QString &query);

  signals:
    void catalogRefreshFailed();
    void searchFailed();
    void launchRejected();
    void launchCompleted();
    void launchFailed();

  private:
    class Impl;

    LauncherController(LauncherControllerOptions options,
                       std::unique_ptr<LauncherIndexBackend> indexBackend,
                       std::unique_ptr<LauncherLaunchBackend> launchBackend);

    void handleLaunchIntent(const ApplicationLaunchIntent &intent);
    void handleRefreshCompletion(
        std::uint64_t generation,
        foundation::Result<std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot>>
            result);
    void handleSearchCompletion(
        std::uint64_t catalogGeneration, std::uint64_t requestGeneration,
        foundation::Result<std::shared_ptr<const prismdrake::launcher::ApplicationSearchSnapshot>>
            result);
    void handleLaunchCompletion(foundation::Result<void> result);
    void startCurrentSearch();

    LauncherControllerOptions options_;
    LauncherPresentationModel presentation_;
    std::shared_ptr<const prismdrake::launcher::ApplicationCatalogSnapshot> catalog_;
    prismdrake::launcher::ApplicationSearchQuery query_;
    std::unique_ptr<Impl> implementation_;
    std::uint64_t catalog_generation_{0U};
    std::uint64_t request_generation_{1U};
};

} // namespace prismdrake::shell::launcher::controller
