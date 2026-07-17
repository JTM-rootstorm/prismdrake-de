#pragma once

#include "ApplicationCatalog.hpp"
#include "Cancellation.hpp"
#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prismdrake::launcher {

inline constexpr std::size_t maximumApplicationSearchQueryBytes = 1024U;
inline constexpr std::size_t maximumApplicationSearchQueryCodepoints = 256U;
inline constexpr std::size_t maximumApplicationSearchQueryTokens = 32U;
inline constexpr std::size_t maximumApplicationSearchWorkUnits = 4096U;
inline constexpr std::size_t maximumApplicationSearchResults = 512U;

/// A validated local-application query with deterministic, locale-neutral matching semantics.
///
/// ASCII letters are folded to lowercase. Non-ASCII UTF-8 is compared exactly; this type makes
/// no Unicode normalization, locale-sensitive case-folding, stemming, or collation claim.
class ApplicationSearchQuery final {
  public:
    [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }
    [[nodiscard]] const std::vector<std::string> &normalizedTokens() const noexcept {
        return tokens_;
    }
    [[nodiscard]] const std::string &normalizedPhrase() const noexcept { return phrase_; }

    friend bool operator==(const ApplicationSearchQuery &,
                           const ApplicationSearchQuery &) = default;

  private:
    ApplicationSearchQuery(std::vector<std::string> tokens, std::string phrase)
        : tokens_(std::move(tokens)), phrase_(std::move(phrase)) {}

    std::vector<std::string> tokens_;
    std::string phrase_;

    friend foundation::Result<ApplicationSearchQuery>
    parseApplicationSearchQuery(std::string_view text);
    friend class ApplicationSearchOperation;
};

enum class ApplicationSearchViewState : std::uint8_t {
    loading,
    results,
    emptyCatalog,
    noResults,
    error,
};

/// One result references the immutable discovery snapshot rather than copying private fields.
struct ApplicationSearchResult final {
    std::size_t discoveryEntryIndex;

    friend bool operator==(const ApplicationSearchResult &,
                           const ApplicationSearchResult &) = default;
};

/// Immutable results for one catalog and request generation.
///
/// A loading snapshot may contain provisional, deterministically ordered results. A caller must
/// reject publications whose catalog or request generation is no longer current.
struct ApplicationSearchSnapshot final {
    std::uint64_t catalogGeneration;
    std::uint64_t requestGeneration;
    ApplicationSearchViewState state;
    std::size_t examinedApplications;
    std::size_t totalApplications;
    std::vector<ApplicationSearchResult> results;
    bool truncated;

    friend bool operator==(const ApplicationSearchSnapshot &,
                           const ApplicationSearchSnapshot &) = default;
};

[[nodiscard]] foundation::Result<ApplicationSearchQuery>
parseApplicationSearchQuery(std::string_view text);

/// Pull-based, single-owner local-application search.
///
/// The operation creates no thread and performs no filesystem, display, provider, or execution
/// work. Callers choose where to schedule each bounded advance.
class ApplicationSearchOperation final {
  public:
    ~ApplicationSearchOperation();

    ApplicationSearchOperation(const ApplicationSearchOperation &) = delete;
    ApplicationSearchOperation &operator=(const ApplicationSearchOperation &) = delete;
    ApplicationSearchOperation(ApplicationSearchOperation &&) noexcept;
    ApplicationSearchOperation &operator=(ApplicationSearchOperation &&) noexcept;

    [[nodiscard]] foundation::Result<std::shared_ptr<const ApplicationSearchSnapshot>>
    advance(std::size_t maximumWorkUnits, const foundation::CancellationToken &cancellation);

  private:
    struct Impl;
    explicit ApplicationSearchOperation(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;

    friend foundation::Result<ApplicationSearchOperation>
    createApplicationSearch(std::shared_ptr<const ApplicationCatalogSnapshot> catalog,
                            std::uint64_t requestGeneration, ApplicationSearchQuery query,
                            std::size_t resultLimit);
};

[[nodiscard]] foundation::Result<ApplicationSearchOperation>
createApplicationSearch(std::shared_ptr<const ApplicationCatalogSnapshot> catalog,
                        std::uint64_t requestGeneration, ApplicationSearchQuery query,
                        std::size_t resultLimit = maximumApplicationSearchResults);

/// Creates the model-level error state used when catalog construction failed.
///
/// The error publication intentionally carries no query, application, desktop-file, or path text.
[[nodiscard]] foundation::Result<std::shared_ptr<const ApplicationSearchSnapshot>>
makeApplicationSearchErrorSnapshot(std::uint64_t catalogGeneration,
                                   std::uint64_t requestGeneration);

} // namespace prismdrake::launcher
