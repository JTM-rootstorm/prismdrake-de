#pragma once

#include "ApplicationPaths.hpp"
#include "Cancellation.hpp"
#include "DesktopEntry.hpp"
#include "DesktopEntryParser.hpp"
#include "DesktopEntryVisibility.hpp"
#include "DesktopFileId.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumDesktopDiscoveryRoots = 128U;
inline constexpr std::size_t maximumDesktopDiscoveryNodesPerRoot = 65536U;
inline constexpr std::size_t maximumDesktopDiscoveryDirectoriesPerRoot = 4096U;
inline constexpr std::size_t maximumDesktopDiscoveryCandidatesPerRoot = 16384U;
inline constexpr std::size_t maximumDesktopDiscoveryEntries = 16384U;
inline constexpr std::size_t maximumDesktopDiscoveryDiagnostics = 256U;
inline constexpr std::size_t maximumDesktopDiscoveryDepth = 64U;
inline constexpr std::size_t maximumDiscoveredDesktopFileLocationBytes = 64U * 1024U;

/// Caller-tunable limits capped by the compile-time discovery envelope.
struct DesktopEntryDiscoveryLimits final {
    std::size_t roots{maximumDesktopDiscoveryRoots};
    std::size_t nodesPerRoot{maximumDesktopDiscoveryNodesPerRoot};
    std::size_t directoriesPerRoot{maximumDesktopDiscoveryDirectoriesPerRoot};
    std::size_t candidatesPerRoot{maximumDesktopDiscoveryCandidatesPerRoot};
    std::size_t entries{maximumDesktopDiscoveryEntries};
    std::size_t diagnostics{maximumDesktopDiscoveryDiagnostics};
    std::size_t depth{maximumDesktopDiscoveryDepth};
};

/// Closed, path-free diagnostic categories emitted by discovery.
enum class DesktopEntryDiscoveryDiagnosticCode : std::uint8_t {
    rootUnavailable,
    directoryUnavailable,
    relativePathRejected,
    fileUnavailable,
    fileTooLarge,
    desktopEntryRejected,
    depthLimitReached,
    directoryLimitReached,
    nodeLimitReached,
    candidateLimitReached,
    entryLimitReached,
};

/// A redacted diagnostic identifies only its category and precedence root.
struct DesktopEntryDiscoveryDiagnostic final {
    DesktopEntryDiscoveryDiagnosticCode code;
    std::size_t rootIndex;

    friend bool operator==(const DesktopEntryDiscoveryDiagnostic &,
                           const DesktopEntryDiscoveryDiagnostic &) = default;
};

/// Validated lexical provenance for one discovered desktop file.
///
/// The absolute path is the exact candidate path used for bounded reads, including an accepted
/// regular-file symlink or a configured root symlink. It is not canonicalized. Relative identity
/// and root precedence remain available for catalog revalidation without exposing paths to QML.
class DiscoveredDesktopFileLocation final {
  public:
    [[nodiscard]] const std::filesystem::path &absolutePath() const noexcept {
        return absolute_path_;
    }
    [[nodiscard]] const std::string &relativePath() const noexcept { return relative_path_; }
    [[nodiscard]] std::size_t rootIndex() const noexcept { return root_index_; }

    friend bool operator==(const DiscoveredDesktopFileLocation &,
                           const DiscoveredDesktopFileLocation &) = default;

  private:
    DiscoveredDesktopFileLocation(std::filesystem::path absolutePath, std::string relativePath,
                                  std::size_t rootIndex)
        : absolute_path_(std::move(absolutePath)), relative_path_(std::move(relativePath)),
          root_index_(rootIndex) {}

    std::filesystem::path absolute_path_;
    std::string relative_path_;
    std::size_t root_index_;

    friend foundation::Result<DiscoveredDesktopFileLocation>
    makeDiscoveredDesktopFileLocation(const std::filesystem::path &, std::string_view, std::size_t);
};

/// Validates lexical root/relative provenance without resolving symlinks or accessing a file.
[[nodiscard]] foundation::Result<DiscoveredDesktopFileLocation>
makeDiscoveredDesktopFileLocation(const std::filesystem::path &applicationRoot,
                                  std::string_view relativePath, std::size_t rootIndex);

/// Revalidates that a retained relative location derives the accompanying desktop-file identity.
[[nodiscard]] foundation::Result<void>
validateDiscoveredDesktopFileLocation(const DesktopFileId &id,
                                      const DiscoveredDesktopFileLocation &location);

/// One successfully parsed desktop entry, including non-launchable tombstones.
struct DiscoveredDesktopEntry final {
    DesktopFileId id;
    DesktopEntry entry;
    DesktopEntryVisibilityReason visibility;
    DiscoveredDesktopFileLocation location;

    friend bool operator==(const DiscoveredDesktopEntry &,
                           const DiscoveredDesktopEntry &) = default;
};

/// Immutable cumulative view published after a caller-controlled work batch.
struct DesktopEntryDiscoverySnapshot final {
    std::vector<DiscoveredDesktopEntry> entries;
    std::vector<std::size_t> visibleEntryIndices;
    std::vector<DesktopEntryDiscoveryDiagnostic> diagnostics;
    std::size_t claimedDesktopFileIds{0U};
    bool complete{false};
    bool truncated{false};
    bool diagnosticsTruncated{false};
};

/// Result of one bounded pull from the authoritative discovery owner.
struct DesktopEntryDiscoveryBatch final {
    std::shared_ptr<const DesktopEntryDiscoverySnapshot> snapshot;
    std::vector<std::size_t> addedEntryIndices;
    std::vector<std::size_t> addedVisibleEntryIndices;
    std::size_t workUnits{0U};
    bool cancellationObserved{false};
};

/// Pull-based, single-owner desktop-entry scanner.
///
/// The scanner creates no threads. A caller supplies both the work budget and
/// cancellation token for every pull and may resume later with another token.
class DesktopEntryDiscovery final {
  public:
    ~DesktopEntryDiscovery();

    DesktopEntryDiscovery(const DesktopEntryDiscovery &) = delete;
    DesktopEntryDiscovery &operator=(const DesktopEntryDiscovery &) = delete;
    DesktopEntryDiscovery(DesktopEntryDiscovery &&) noexcept;
    DesktopEntryDiscovery &operator=(DesktopEntryDiscovery &&) noexcept;

    [[nodiscard]] foundation::Result<DesktopEntryDiscoveryBatch>
    pull(std::size_t maximumWorkUnits, const foundation::CancellationToken &cancellation);

  private:
    struct Impl;
    explicit DesktopEntryDiscovery(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;

    friend foundation::Result<DesktopEntryDiscovery>
        createDesktopEntryDiscovery(ApplicationPaths, DesktopEntryParseContext,
                                    CurrentDesktopContext, DesktopEntryDiscoveryLimits);
};

/// Creates a bounded scanner without touching the filesystem.
[[nodiscard]] foundation::Result<DesktopEntryDiscovery>
createDesktopEntryDiscovery(ApplicationPaths paths, DesktopEntryParseContext parseContext,
                            CurrentDesktopContext currentDesktop,
                            DesktopEntryDiscoveryLimits limits = {});

} // namespace prismdrake::launcher
