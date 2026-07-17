#include "ApplicationCatalog.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] Error cancelledError() {
    return {ErrorCode::cancelled, "Application catalog eligibility was cancelled.",
            "Discard the stale operation and create a catalog for the current discovery input."};
}

[[nodiscard]] ApplicationCatalogEligibilityReason
catalogReason(DesktopTryExecEligibilityReason reason) noexcept {
    switch (reason) {
    case DesktopTryExecEligibilityReason::eligibleWithoutTryExec:
        return ApplicationCatalogEligibilityReason::eligibleWithoutTryExec;
    case DesktopTryExecEligibilityReason::eligibleExecutable:
        return ApplicationCatalogEligibilityReason::eligibleTryExec;
    case DesktopTryExecEligibilityReason::ineligibleMissing:
        return ApplicationCatalogEligibilityReason::excludedTryExecMissing;
    case DesktopTryExecEligibilityReason::ineligibleNotRegularFile:
        return ApplicationCatalogEligibilityReason::excludedTryExecNotRegularFile;
    case DesktopTryExecEligibilityReason::ineligibleNotExecutable:
        return ApplicationCatalogEligibilityReason::excludedTryExecNotExecutable;
    }
    return ApplicationCatalogEligibilityReason::excludedTryExecMissing;
}

[[nodiscard]] Result<void> validateDiscovery(const DesktopEntryDiscoverySnapshot &discovery) {
    if (discovery.entries.size() > maximumDesktopDiscoveryEntries ||
        discovery.visibleEntryIndices.size() > maximumDesktopDiscoveryEntries ||
        discovery.diagnostics.size() > maximumDesktopDiscoveryDiagnostics) {
        return Result<void>::failure(
            {ErrorCode::too_large, "The application catalog discovery snapshot is too large.",
             "Rebuild discovery within its documented entry and diagnostic limits."});
    }

    std::unordered_set<std::size_t> visibleIndices;
    std::unordered_set<std::string> desktopFileIds;
    visibleIndices.reserve(discovery.visibleEntryIndices.size());
    desktopFileIds.reserve(discovery.entries.size());
    for (const auto &entry : discovery.entries) {
        auto location = validateDiscoveredDesktopFileLocation(entry.id, entry.location);
        if (!location) {
            return location;
        }
        if (!desktopFileIds.insert(entry.id.value()).second) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "The application catalog discovery snapshot contains duplicate identifiers.",
                 "Rebuild one authoritative immutable discovery snapshot."});
        }
    }
    for (const auto index : discovery.visibleEntryIndices) {
        if (index >= discovery.entries.size() || !visibleIndices.insert(index).second ||
            !isVisible(discovery.entries[index].visibility)) {
            return Result<void>::failure(
                {ErrorCode::validation_error,
                 "The application catalog discovery visibility index is invalid.",
                 "Rebuild one authoritative immutable discovery snapshot."});
        }
    }
    return Result<void>::success();
}

} // namespace

struct ApplicationCatalogOperation::Impl final {
    Impl(std::shared_ptr<const DesktopEntryDiscoverySnapshot> source,
         DesktopExecutableLookupContext executableLookup, std::uint64_t sourceGeneration)
        : discovery(std::move(source)), lookupContext(std::move(executableLookup)),
          generation(sourceGeneration) {}

    std::shared_ptr<const DesktopEntryDiscoverySnapshot> discovery;
    DesktopExecutableLookupContext lookupContext;
    std::uint64_t generation;
    std::size_t cursor{0U};
    std::vector<ApplicationCatalogDecision> decisions;
    std::vector<std::size_t> eligibleEntryIndices;
    std::shared_ptr<const ApplicationCatalogSnapshot> published;
    std::optional<Error> terminalError;
    bool dirty{true};

    [[nodiscard]] std::shared_ptr<const ApplicationCatalogSnapshot> publish() {
        if (dirty || !published) {
            const bool complete =
                discovery->complete && cursor == discovery->visibleEntryIndices.size();
            published =
                std::make_shared<const ApplicationCatalogSnapshot>(ApplicationCatalogSnapshot{
                    generation, discovery, decisions, eligibleEntryIndices, cursor,
                    discovery->visibleEntryIndices.size(), complete});
            dirty = false;
        }
        return published;
    }

    void terminate(Error error) { terminalError = std::move(error); }
};

ApplicationCatalogOperation::ApplicationCatalogOperation(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

ApplicationCatalogOperation::~ApplicationCatalogOperation() = default;
ApplicationCatalogOperation::ApplicationCatalogOperation(ApplicationCatalogOperation &&) noexcept =
    default;
ApplicationCatalogOperation &
ApplicationCatalogOperation::operator=(ApplicationCatalogOperation &&) noexcept = default;

Result<ApplicationCatalogBatch>
ApplicationCatalogOperation::pull(std::size_t maximumWorkUnits,
                                  const foundation::CancellationToken &cancellation) {
    if (!implementation_) {
        return failure<ApplicationCatalogBatch>(ErrorCode::invalid_argument,
                                                "The application catalog owner was moved from.",
                                                "Use the destination catalog owner.");
    }
    if (maximumWorkUnits == 0U || maximumWorkUnits > maximumApplicationCatalogWorkUnits) {
        return failure<ApplicationCatalogBatch>(
            ErrorCode::invalid_argument, "The application catalog work budget is invalid.",
            "Use a nonzero work budget within the documented catalog limit.");
    }
    if (implementation_->terminalError) {
        return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
    }
    if (cancellation.isCancellationRequested()) {
        implementation_->terminate(cancelledError());
        return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
    }

    ApplicationCatalogBatch batch;
    while (batch.workUnits < maximumWorkUnits &&
           implementation_->cursor < implementation_->discovery->visibleEntryIndices.size()) {
        if (cancellation.isCancellationRequested()) {
            implementation_->terminate(cancelledError());
            return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
        }
        const auto entryIndex =
            implementation_->discovery->visibleEntryIndices[implementation_->cursor];
        const auto &entry = implementation_->discovery->entries[entryIndex].entry;
        auto eligibility = evaluateDesktopTryExec(entry.tryExec, implementation_->lookupContext);
        if (!eligibility) {
            implementation_->terminate(eligibility.error());
            return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
        }
        if (cancellation.isCancellationRequested()) {
            implementation_->terminate(cancelledError());
            return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
        }

        const auto reason = catalogReason(eligibility.value().reason);
        implementation_->decisions.push_back({entryIndex, reason});
        if (isCatalogEligible(reason)) {
            implementation_->eligibleEntryIndices.push_back(entryIndex);
        }
        ++implementation_->cursor;
        ++batch.workUnits;
        implementation_->dirty = true;
    }
    if (cancellation.isCancellationRequested()) {
        implementation_->terminate(cancelledError());
        return Result<ApplicationCatalogBatch>::failure(*implementation_->terminalError);
    }
    batch.snapshot = implementation_->publish();
    return Result<ApplicationCatalogBatch>::success(std::move(batch));
}

Result<ApplicationCatalogOperation>
createApplicationCatalog(std::shared_ptr<const DesktopEntryDiscoverySnapshot> discovery,
                         DesktopExecutableLookupContext lookupContext, std::uint64_t generation) {
    if (!discovery) {
        return failure<ApplicationCatalogOperation>(
            ErrorCode::invalid_argument, "The application catalog discovery snapshot is missing.",
            "Provide one immutable desktop-entry discovery snapshot.");
    }
    if (generation == 0U) {
        return failure<ApplicationCatalogOperation>(
            ErrorCode::invalid_argument, "The application catalog generation is invalid.",
            "Use a nonzero generation for a published discovery input.");
    }
    auto discoveryValidation = validateDiscovery(*discovery);
    if (!discoveryValidation) {
        return Result<ApplicationCatalogOperation>::failure(discoveryValidation.error());
    }
    auto lookupValidation = evaluateDesktopTryExec(std::nullopt, lookupContext);
    if (!lookupValidation) {
        return Result<ApplicationCatalogOperation>::failure(lookupValidation.error());
    }
    return Result<ApplicationCatalogOperation>::success(
        ApplicationCatalogOperation{std::make_unique<ApplicationCatalogOperation::Impl>(
            std::move(discovery), std::move(lookupContext), generation)});
}

} // namespace prismdrake::launcher
