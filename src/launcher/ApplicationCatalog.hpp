#pragma once

#include "Cancellation.hpp"
#include "DesktopEntryDiscovery.hpp"
#include "DesktopExecutable.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumApplicationCatalogWorkUnits = 4096U;

/// Closed eligibility decision for one visible discovery entry.
enum class ApplicationCatalogEligibilityReason : std::uint8_t {
    eligibleWithoutTryExec,
    eligibleTryExec,
    excludedTryExecMissing,
    excludedTryExecNotRegularFile,
    excludedTryExecNotExecutable,
};

[[nodiscard]] constexpr bool
isCatalogEligible(ApplicationCatalogEligibilityReason reason) noexcept {
    return reason == ApplicationCatalogEligibilityReason::eligibleWithoutTryExec ||
           reason == ApplicationCatalogEligibilityReason::eligibleTryExec;
}

struct ApplicationCatalogDecision final {
    std::size_t discoveryEntryIndex;
    ApplicationCatalogEligibilityReason reason;

    friend bool operator==(const ApplicationCatalogDecision &,
                           const ApplicationCatalogDecision &) = default;
};

/// Immutable cumulative eligibility view for one discovery generation.
struct ApplicationCatalogSnapshot final {
    std::uint64_t generation;
    std::shared_ptr<const DesktopEntryDiscoverySnapshot> discovery;
    std::vector<ApplicationCatalogDecision> decisions;
    std::vector<std::size_t> eligibleEntryIndices;
    std::size_t examinedEntries;
    std::size_t totalVisibleEntries;
    bool complete;
};

struct ApplicationCatalogBatch final {
    std::shared_ptr<const ApplicationCatalogSnapshot> snapshot;
    std::size_t workUnits{0U};
};

/// Pull-based, single-owner TryExec eligibility operation.
///
/// The operation creates no thread. Each work unit evaluates optional TryExec
/// for exactly one visible discovery entry. It neither expands nor resolves the
/// actual Exec value and never launches a process. Cancellation and resolver
/// failures are terminal for the operation so stale partial work cannot later
/// be published as current.
class ApplicationCatalogOperation final {
  public:
    ~ApplicationCatalogOperation();

    ApplicationCatalogOperation(const ApplicationCatalogOperation &) = delete;
    ApplicationCatalogOperation &operator=(const ApplicationCatalogOperation &) = delete;
    ApplicationCatalogOperation(ApplicationCatalogOperation &&) noexcept;
    ApplicationCatalogOperation &operator=(ApplicationCatalogOperation &&) noexcept;

    [[nodiscard]] foundation::Result<ApplicationCatalogBatch>
    pull(std::size_t maximumWorkUnits, const foundation::CancellationToken &cancellation);

  private:
    struct Impl;
    explicit ApplicationCatalogOperation(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;

    friend foundation::Result<ApplicationCatalogOperation>
    createApplicationCatalog(std::shared_ptr<const DesktopEntryDiscoverySnapshot>,
                             DesktopExecutableLookupContext, std::uint64_t);
};

/// Creates a catalog operation after validating the immutable discovery shape,
/// generation, limits, and complete explicit executable lookup envelope.
[[nodiscard]] foundation::Result<ApplicationCatalogOperation>
createApplicationCatalog(std::shared_ptr<const DesktopEntryDiscoverySnapshot> discovery,
                         DesktopExecutableLookupContext lookupContext, std::uint64_t generation);

} // namespace prismdrake::launcher
