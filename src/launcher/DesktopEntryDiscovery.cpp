#include "DesktopEntryDiscovery.hpp"

#include "BoundedFile.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class DiscoveryPhase : std::uint8_t { startRoot, scanRoot, processCandidates, finished };

struct PendingDirectory final {
    std::filesystem::path absolutePath;
    std::filesystem::path relativePath;
    std::size_t depth;
};

struct Candidate final {
    std::filesystem::path absolutePath;
    std::string relativePath;
};

[[nodiscard]] Result<DesktopEntryDiscovery> failure(ErrorCode code, std::string message,
                                                    std::string recovery) {
    return Result<DesktopEntryDiscovery>::failure(
        Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool containsNull(const std::filesystem::path &path) {
    const auto &native = path.native();
    return std::find(native.begin(), native.end(), typename std::filesystem::path::value_type{}) !=
           native.end();
}

[[nodiscard]] bool containsUnsafeControl(std::string_view value) noexcept {
    return std::ranges::any_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20U || byte == 0x7fU;
    });
}

[[nodiscard]] bool hasDotTraversal(const std::filesystem::path &path) {
    return std::ranges::any_of(path, [](const auto &component) {
        return component == std::filesystem::path{"."} || component == std::filesystem::path{".."};
    });
}

[[nodiscard]] bool limitsAreValid(const DesktopEntryDiscoveryLimits &limits) noexcept {
    return limits.roots > 0U && limits.roots <= maximumDesktopDiscoveryRoots &&
           limits.nodesPerRoot > 0U && limits.nodesPerRoot <= maximumDesktopDiscoveryNodesPerRoot &&
           limits.directoriesPerRoot > 0U &&
           limits.directoriesPerRoot <= maximumDesktopDiscoveryDirectoriesPerRoot &&
           limits.candidatesPerRoot > 0U &&
           limits.candidatesPerRoot <= maximumDesktopDiscoveryCandidatesPerRoot &&
           limits.entries > 0U && limits.entries <= maximumDesktopDiscoveryEntries &&
           limits.diagnostics > 0U && limits.diagnostics <= maximumDesktopDiscoveryDiagnostics &&
           limits.depth > 0U && limits.depth <= maximumDesktopDiscoveryDepth;
}

[[nodiscard]] DesktopEntryDiscoveryDiagnosticCode fileDiagnostic(ErrorCode code) noexcept {
    return code == ErrorCode::too_large ? DesktopEntryDiscoveryDiagnosticCode::fileTooLarge
                                        : DesktopEntryDiscoveryDiagnosticCode::fileUnavailable;
}

} // namespace

Result<DiscoveredDesktopFileLocation>
makeDiscoveredDesktopFileLocation(const std::filesystem::path &applicationRoot,
                                  std::string_view relativePath, std::size_t rootIndex) {
    auto identifier = deriveDesktopFileId(relativePath);
    if (!identifier) {
        return Result<DiscoveredDesktopFileLocation>::failure(identifier.error());
    }
    if (rootIndex >= maximumDesktopDiscoveryRoots || applicationRoot.empty() ||
        !applicationRoot.is_absolute() || containsNull(applicationRoot) ||
        hasDotTraversal(applicationRoot) || containsUnsafeControl(applicationRoot.native()) ||
        containsUnsafeControl(relativePath)) {
        return Result<DiscoveredDesktopFileLocation>::failure(
            {ErrorCode::invalid_argument, "The discovered desktop-file location is invalid.",
             "Use bounded absolute roots and relative desktop paths without controls or "
             "traversal."});
    }
    auto absolute = (applicationRoot / std::filesystem::path{relativePath}).lexically_normal();
    if (!absolute.is_absolute() ||
        absolute.native().size() > maximumDiscoveredDesktopFileLocationBytes) {
        return Result<DiscoveredDesktopFileLocation>::failure(
            {ErrorCode::too_large, "The discovered desktop-file location exceeds its limit.",
             "Use a shorter application root and relative desktop path."});
    }
    return Result<DiscoveredDesktopFileLocation>::success(
        DiscoveredDesktopFileLocation{std::move(absolute), std::string{relativePath}, rootIndex});
}

Result<void> validateDiscoveredDesktopFileLocation(const DesktopFileId &id,
                                                   const DiscoveredDesktopFileLocation &location) {
    auto derived = deriveDesktopFileId(location.relativePath());
    if (!derived || derived.value() != id) {
        return Result<void>::failure(
            {ErrorCode::validation_error,
             "The discovered desktop-file identity and location do not match.",
             "Rebuild one authoritative discovery snapshot from matching path provenance."});
    }
    return Result<void>::success();
}

struct DesktopEntryDiscovery::Impl final {
    Impl(ApplicationPaths applicationPaths, DesktopEntryParseContext entryParseContext,
         CurrentDesktopContext desktopContext, DesktopEntryDiscoveryLimits discoveryLimits)
        : paths(std::move(applicationPaths)), parseContext(std::move(entryParseContext)),
          currentDesktop(std::move(desktopContext)), limits(discoveryLimits) {}

    ApplicationPaths paths;
    DesktopEntryParseContext parseContext;
    CurrentDesktopContext currentDesktop;
    DesktopEntryDiscoveryLimits limits;

    DesktopEntryDiscoverySnapshot snapshot;
    std::shared_ptr<const DesktopEntryDiscoverySnapshot> published;
    std::unordered_set<std::string> claimedIds;

    DiscoveryPhase phase{DiscoveryPhase::startRoot};
    std::size_t rootIndex{0U};
    std::vector<PendingDirectory> pendingDirectories;
    std::optional<PendingDirectory> activeDirectory;
    std::optional<std::filesystem::directory_iterator> directoryIterator;
    std::filesystem::directory_iterator directoryEnd;
    std::vector<std::filesystem::directory_entry> directoryEntries;
    std::size_t directoryEntryIndex{0U};
    std::vector<Candidate> candidates;
    std::size_t candidateIndex{0U};
    std::size_t nodesInRoot{0U};
    std::size_t directoriesInRoot{0U};
    bool stopAfterRoot{false};
    bool dirty{true};

    void addDiagnostic(DesktopEntryDiscoveryDiagnosticCode code) {
        if (snapshot.diagnostics.size() < limits.diagnostics) {
            snapshot.diagnostics.push_back({code, rootIndex});
            dirty = true;
        } else if (!snapshot.diagnosticsTruncated) {
            snapshot.diagnosticsTruncated = true;
            dirty = true;
        }
    }

    void markTruncated(DesktopEntryDiscoveryDiagnosticCode code) {
        snapshot.truncated = true;
        stopAfterRoot = true;
        addDiagnostic(code);
    }

    void finish() {
        phase = DiscoveryPhase::finished;
        snapshot.complete = true;
        dirty = true;
    }

    void advanceRoot() {
        pendingDirectories.clear();
        activeDirectory.reset();
        directoryIterator.reset();
        directoryEntries.clear();
        directoryEntryIndex = 0U;
        candidates.clear();
        candidateIndex = 0U;
        nodesInRoot = 0U;
        directoriesInRoot = 0U;
        stopAfterRoot = false;
        ++rootIndex;
        if (rootIndex >= paths.applicationDirectories.size()) {
            finish();
        } else {
            phase = DiscoveryPhase::startRoot;
        }
    }

    void startRoot() {
        const auto &root = paths.applicationDirectories[rootIndex];
        std::error_code error;
        const auto rootStatus = std::filesystem::status(root, error);
        if (error == std::errc::no_such_file_or_directory ||
            rootStatus.type() == std::filesystem::file_type::not_found) {
            advanceRoot();
            return;
        }
        if (error || !std::filesystem::is_directory(rootStatus)) {
            addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::rootUnavailable);
            snapshot.truncated = true;
            finish();
            return;
        }

        pendingDirectories.push_back({root, {}, 0U});
        directoriesInRoot = 1U;
        phase = DiscoveryPhase::scanRoot;
    }

    void keepLexicallySmallestCandidate(Candidate candidate) {
        if (candidates.size() < limits.candidatesPerRoot) {
            candidates.push_back(std::move(candidate));
            return;
        }

        markTruncated(DesktopEntryDiscoveryDiagnosticCode::candidateLimitReached);
        const auto largest = std::ranges::max_element(candidates, {}, &Candidate::relativePath);
        if (candidate.relativePath < largest->relativePath) {
            *largest = std::move(candidate);
        }
    }

    void inspectDirectoryEntry(const std::filesystem::directory_entry &entry,
                               const PendingDirectory &directory) {
        const auto name = entry.path().filename();
        const auto relative = directory.relativePath / name;
        std::error_code error;
        const auto linkStatus = entry.symlink_status(error);
        if (error) {
            addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::directoryUnavailable);
            return;
        }

        if (!std::filesystem::is_symlink(linkStatus) && std::filesystem::is_directory(linkStatus)) {
            const auto depth = directory.depth + 1U;
            if (depth >= limits.depth) {
                markTruncated(DesktopEntryDiscoveryDiagnosticCode::depthLimitReached);
                return;
            }
            if (directoriesInRoot >= limits.directoriesPerRoot) {
                markTruncated(DesktopEntryDiscoveryDiagnosticCode::directoryLimitReached);
                return;
            }
            ++directoriesInRoot;
            pendingDirectories.push_back({entry.path(), relative, depth});
            return;
        }

        const auto targetStatus = entry.status(error);
        if (error || !std::filesystem::is_regular_file(targetStatus)) {
            return;
        }
        const auto relativeString = relative.generic_string();
        if (!relativeString.ends_with(".desktop")) {
            return;
        }
        keepLexicallySmallestCandidate({entry.path(), relativeString});
    }

    void scanRoot() {
        if (!activeDirectory) {
            if (pendingDirectories.empty()) {
                std::ranges::sort(candidates, {}, &Candidate::relativePath);
                candidateIndex = 0U;
                phase = DiscoveryPhase::processCandidates;
                return;
            }

            const auto nextDirectory = std::ranges::min_element(
                pendingDirectories, {}, [](const PendingDirectory &directory) {
                    return directory.relativePath.generic_string();
                });
            auto directory = std::move(*nextDirectory);
            pendingDirectories.erase(nextDirectory);
            std::error_code error;
            if (directory.depth > 0U) {
                const auto directoryStatus =
                    std::filesystem::symlink_status(directory.absolutePath, error);
                if (error || std::filesystem::is_symlink(directoryStatus) ||
                    !std::filesystem::is_directory(directoryStatus)) {
                    addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::directoryUnavailable);
                    snapshot.truncated = true;
                    stopAfterRoot = true;
                    candidates.clear();
                    pendingDirectories.clear();
                    directoryEntries.clear();
                    candidateIndex = 0U;
                    phase = DiscoveryPhase::processCandidates;
                    return;
                }
            }
            directoryIterator.emplace(directory.absolutePath, error);
            if (error) {
                directoryIterator.reset();
                addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::directoryUnavailable);
                snapshot.truncated = true;
                stopAfterRoot = true;
                candidates.clear();
                pendingDirectories.clear();
                directoryEntries.clear();
                candidateIndex = 0U;
                phase = DiscoveryPhase::processCandidates;
                return;
            }
            activeDirectory = std::move(directory);
            directoryEntries.clear();
            directoryEntryIndex = 0U;
            return;
        }

        if (directoryIterator) {
            if (*directoryIterator == directoryEnd) {
                directoryIterator.reset();
                std::ranges::sort(directoryEntries, {}, [](const auto &entry) {
                    return entry.path().filename().generic_string();
                });
                return;
            }

            // If a directory contains more than the remaining node envelope,
            // publish no partial result for this root. That keeps the bounded
            // outcome independent from unspecified filesystem enumeration.
            if (nodesInRoot >= limits.nodesPerRoot) {
                markTruncated(DesktopEntryDiscoveryDiagnosticCode::nodeLimitReached);
                candidates.clear();
                pendingDirectories.clear();
                activeDirectory.reset();
                directoryIterator.reset();
                directoryEntries.clear();
                phase = DiscoveryPhase::processCandidates;
                candidateIndex = 0U;
                return;
            }

            directoryEntries.push_back(**directoryIterator);
            ++nodesInRoot;
            std::error_code incrementError;
            directoryIterator->increment(incrementError);
            if (incrementError) {
                directoryIterator.reset();
                directoryEntries.clear();
                activeDirectory.reset();
                pendingDirectories.clear();
                candidates.clear();
                addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::directoryUnavailable);
                snapshot.truncated = true;
                stopAfterRoot = true;
                phase = DiscoveryPhase::processCandidates;
                candidateIndex = 0U;
            }
            return;
        }

        if (directoryEntryIndex < directoryEntries.size()) {
            inspectDirectoryEntry(directoryEntries[directoryEntryIndex++], *activeDirectory);
            return;
        }
        directoryEntries.clear();
        directoryEntryIndex = 0U;
        activeDirectory.reset();
    }

    void processCandidate() {
        if (candidateIndex >= candidates.size()) {
            if (stopAfterRoot) {
                finish();
            } else {
                advanceRoot();
            }
            return;
        }

        const auto &candidate = candidates[candidateIndex++];
        auto id = deriveDesktopFileId(candidate.relativePath);
        if (!id) {
            addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::relativePathRejected);
            return;
        }
        if (!claimedIds.insert(id.value().value()).second) {
            return;
        }
        snapshot.claimedDesktopFileIds = claimedIds.size();
        dirty = true;

        const auto contents =
            foundation::readBoundedFile(candidate.absolutePath, maximumDesktopEntryFileBytes);
        if (!contents) {
            addDiagnostic(fileDiagnostic(contents.error().code));
            return;
        }
        auto entry = parseDesktopEntry(contents.value(), parseContext);
        if (!entry) {
            addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::desktopEntryRejected);
            return;
        }
        if (snapshot.entries.size() >= limits.entries) {
            markTruncated(DesktopEntryDiscoveryDiagnosticCode::entryLimitReached);
            finish();
            return;
        }

        const auto visibility = evaluateDesktopEntryVisibility(entry.value(), currentDesktop);
        auto location = makeDiscoveredDesktopFileLocation(paths.applicationDirectories[rootIndex],
                                                          candidate.relativePath, rootIndex);
        if (!location) {
            addDiagnostic(DesktopEntryDiscoveryDiagnosticCode::relativePathRejected);
            return;
        }
        snapshot.entries.push_back({std::move(id).value(), std::move(entry).value(), visibility,
                                    std::move(location).value()});
        const auto index = snapshot.entries.size() - 1U;
        if (isVisible(visibility)) {
            snapshot.visibleEntryIndices.push_back(index);
        }
        dirty = true;
    }

    void step() {
        switch (phase) {
        case DiscoveryPhase::startRoot:
            startRoot();
            break;
        case DiscoveryPhase::scanRoot:
            scanRoot();
            break;
        case DiscoveryPhase::processCandidates:
            processCandidate();
            break;
        case DiscoveryPhase::finished:
            break;
        }
    }

    [[nodiscard]] std::shared_ptr<const DesktopEntryDiscoverySnapshot> publish() {
        if (dirty || !published) {
            published = std::make_shared<const DesktopEntryDiscoverySnapshot>(snapshot);
            dirty = false;
        }
        return published;
    }
};

DesktopEntryDiscovery::DesktopEntryDiscovery(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

DesktopEntryDiscovery::~DesktopEntryDiscovery() = default;
DesktopEntryDiscovery::DesktopEntryDiscovery(DesktopEntryDiscovery &&) noexcept = default;
DesktopEntryDiscovery &
DesktopEntryDiscovery::operator=(DesktopEntryDiscovery &&) noexcept = default;

Result<DesktopEntryDiscoveryBatch>
DesktopEntryDiscovery::pull(std::size_t maximumWorkUnits,
                            const foundation::CancellationToken &cancellation) {
    if (!implementation_) {
        return Result<DesktopEntryDiscoveryBatch>::failure(
            {ErrorCode::invalid_argument, "The discovery owner has been moved from.",
             "Use the destination discovery owner."});
    }
    if (maximumWorkUnits == 0U) {
        return Result<DesktopEntryDiscoveryBatch>::failure(
            {ErrorCode::invalid_argument, "The discovery work budget must be positive.",
             "Provide at least one work unit."});
    }

    DesktopEntryDiscoveryBatch batch;
    const auto initialEntries = implementation_->snapshot.entries.size();
    const auto initialVisible = implementation_->snapshot.visibleEntryIndices.size();
    while (batch.workUnits < maximumWorkUnits &&
           implementation_->phase != DiscoveryPhase::finished) {
        if (cancellation.isCancellationRequested()) {
            batch.cancellationObserved = true;
            break;
        }
        implementation_->step();
        ++batch.workUnits;
    }

    for (std::size_t index = initialEntries; index < implementation_->snapshot.entries.size();
         ++index) {
        batch.addedEntryIndices.push_back(index);
    }
    for (std::size_t index = initialVisible;
         index < implementation_->snapshot.visibleEntryIndices.size(); ++index) {
        batch.addedVisibleEntryIndices.push_back(
            implementation_->snapshot.visibleEntryIndices[index]);
    }
    batch.snapshot = implementation_->publish();
    return Result<DesktopEntryDiscoveryBatch>::success(std::move(batch));
}

Result<DesktopEntryDiscovery> createDesktopEntryDiscovery(ApplicationPaths paths,
                                                          DesktopEntryParseContext parseContext,
                                                          CurrentDesktopContext currentDesktop,
                                                          DesktopEntryDiscoveryLimits limits) {
    if (!limitsAreValid(limits)) {
        return failure(ErrorCode::invalid_argument,
                       "Desktop-entry discovery limits are outside the supported envelope.",
                       "Use positive limits no larger than the documented maxima.");
    }
    if (paths.applicationDirectories.size() > limits.roots) {
        return failure(ErrorCode::too_large,
                       "The application-root list exceeds the configured discovery limit.",
                       "Use fewer application roots or a reviewed larger limit.");
    }
    for (const auto &root : paths.applicationDirectories) {
        if (root.empty() || !root.is_absolute() || containsNull(root) ||
            root.native().size() > maximumDesktopFileRelativePathBytes || hasDotTraversal(root)) {
            return failure(ErrorCode::invalid_argument,
                           "An application root is invalid for bounded discovery.",
                           "Use bounded absolute roots without nulls or dot traversal.");
        }
    }

    auto implementation = std::make_unique<DesktopEntryDiscovery::Impl>(
        std::move(paths), std::move(parseContext), std::move(currentDesktop), limits);
    if (implementation->paths.applicationDirectories.empty()) {
        implementation->finish();
    }
    return Result<DesktopEntryDiscovery>::success(DesktopEntryDiscovery{std::move(implementation)});
}

} // namespace prismdrake::launcher
