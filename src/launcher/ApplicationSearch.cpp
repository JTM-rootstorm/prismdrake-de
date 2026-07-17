#include "ApplicationSearch.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class Utf8Status : std::uint8_t {
    valid,
    invalid,
    tooManyCodepoints,
    unsafeControl,
};

enum class MatchShape : std::uint8_t {
    exact,
    prefix,
    wordPrefix,
    substring,
};

enum class SearchField : std::uint8_t {
    name,
    genericName,
    keyword,
    category,
};

inline constexpr std::uint8_t noMatchRank = std::numeric_limits<std::uint8_t>::max();

[[nodiscard]] Result<ApplicationSearchQuery> queryFailure(ErrorCode code, std::string message,
                                                          std::string recovery) {
    return Result<ApplicationSearchQuery>::failure(
        Error{code, std::move(message), std::move(recovery)});
}

template <typename Value>
[[nodiscard]] Result<Value> operationFailure(ErrorCode code, std::string message,
                                             std::string recovery) {
    return Result<Value>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool isAsciiWhitespace(unsigned char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' ||
           byte == '\v';
}

[[nodiscard]] bool isAsciiWordByte(unsigned char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80U;
}

[[nodiscard]] Utf8Status validateQueryUtf8(std::string_view value) noexcept {
    std::size_t codepoints = 0U;
    for (std::size_t offset = 0U; offset < value.size();) {
        const auto lead = static_cast<unsigned char>(value[offset]);
        std::size_t width = 0U;
        std::uint32_t codepoint = 0U;
        if (lead <= 0x7FU) {
            width = 1U;
            codepoint = lead;
        } else if ((lead & 0xE0U) == 0xC0U) {
            width = 2U;
            codepoint = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            width = 3U;
            codepoint = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            width = 4U;
            codepoint = lead & 0x07U;
        } else {
            return Utf8Status::invalid;
        }
        if (offset + width > value.size()) {
            return Utf8Status::invalid;
        }
        for (std::size_t continuation = 1U; continuation < width; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[offset + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return Utf8Status::invalid;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3FU);
        }
        if ((width == 2U && codepoint < 0x80U) || (width == 3U && codepoint < 0x800U) ||
            (width == 4U && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
            return Utf8Status::invalid;
        }
        if (codepoint == 0U || (!isAsciiWhitespace(lead) && codepoint < 0x20U) ||
            (codepoint >= 0x7FU && codepoint <= 0x9FU)) {
            return Utf8Status::unsafeControl;
        }
        ++codepoints;
        if (codepoints > maximumApplicationSearchQueryCodepoints) {
            return Utf8Status::tooManyCodepoints;
        }
        offset += width;
    }
    return Utf8Status::valid;
}

[[nodiscard]] std::string foldAscii(std::string_view value) {
    std::string folded{value};
    for (char &character : folded) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z') {
            character = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return folded;
}

[[nodiscard]] std::optional<MatchShape> matchShape(std::string_view field,
                                                   std::string_view needle) noexcept {
    if (field == needle) {
        return MatchShape::exact;
    }
    if (field.starts_with(needle)) {
        return MatchShape::prefix;
    }
    auto position = field.find(needle);
    while (position != std::string_view::npos) {
        if (position > 0U && !isAsciiWordByte(static_cast<unsigned char>(field[position - 1U]))) {
            return MatchShape::wordPrefix;
        }
        position = field.find(needle, position + 1U);
    }
    return field.find(needle) == std::string_view::npos
               ? std::nullopt
               : std::optional<MatchShape>{MatchShape::substring};
}

[[nodiscard]] constexpr std::uint8_t rank(MatchShape shape, SearchField field) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(shape) * 4U +
                                     static_cast<std::uint8_t>(field));
}

void considerField(std::uint8_t &best, std::string_view foldedField, std::string_view needle,
                   SearchField field) {
    const auto shape = matchShape(foldedField, needle);
    if (shape) {
        best = std::min(best, rank(*shape, field));
    }
}

[[nodiscard]] std::uint8_t bestMatch(const DesktopEntry &entry, std::string_view needle,
                                     const foundation::CancellationToken &cancellation,
                                     bool &cancelled) {
    std::uint8_t best = noMatchRank;
    if (entry.name) {
        considerField(best, foldAscii(*entry.name), needle, SearchField::name);
    }
    if (entry.genericName) {
        considerField(best, foldAscii(*entry.genericName), needle, SearchField::genericName);
    }
    for (const auto &keyword : entry.keywords) {
        if (cancellation.isCancellationRequested()) {
            cancelled = true;
            return noMatchRank;
        }
        considerField(best, foldAscii(keyword), needle, SearchField::keyword);
    }
    for (const auto &category : entry.categories) {
        if (cancellation.isCancellationRequested()) {
            cancelled = true;
            return noMatchRank;
        }
        considerField(best, foldAscii(category), needle, SearchField::category);
    }
    return best;
}

struct MatchScore final {
    std::uint8_t phraseRank;
    std::uint16_t tokenRankSum;
    std::uint8_t worstTokenRank;
    std::string foldedName;
    std::string desktopFileId;
    std::size_t entryIndex;
};

[[nodiscard]] bool scoreLess(const MatchScore &left, const MatchScore &right) noexcept {
    if (left.phraseRank != right.phraseRank) {
        return left.phraseRank < right.phraseRank;
    }
    if (left.tokenRankSum != right.tokenRankSum) {
        return left.tokenRankSum < right.tokenRankSum;
    }
    if (left.worstTokenRank != right.worstTokenRank) {
        return left.worstTokenRank < right.worstTokenRank;
    }
    if (left.foldedName != right.foldedName) {
        return left.foldedName < right.foldedName;
    }
    if (left.desktopFileId != right.desktopFileId) {
        return left.desktopFileId < right.desktopFileId;
    }
    return left.entryIndex < right.entryIndex;
}

[[nodiscard]] std::optional<MatchScore>
scoreEntry(const DiscoveredDesktopEntry &discovered, std::size_t entryIndex,
           const ApplicationSearchQuery &query, const foundation::CancellationToken &cancellation,
           bool &cancelled) {
    MatchScore score{
        noMatchRank,           0U,        0U, foldAscii(discovered.entry.name.value_or("")),
        discovered.id.value(), entryIndex};
    if (query.empty()) {
        score.phraseRank = 0U;
        return score;
    }

    score.phraseRank =
        bestMatch(discovered.entry, query.normalizedPhrase(), cancellation, cancelled);
    if (cancelled) {
        return std::nullopt;
    }
    for (const auto &token : query.normalizedTokens()) {
        const auto tokenRank = bestMatch(discovered.entry, token, cancellation, cancelled);
        if (cancelled || tokenRank == noMatchRank) {
            return std::nullopt;
        }
        score.tokenRankSum = static_cast<std::uint16_t>(score.tokenRankSum + tokenRank);
        score.worstTokenRank = std::max(score.worstTokenRank, tokenRank);
    }
    return score;
}

[[nodiscard]] bool generationsAreValid(std::uint64_t catalogGeneration,
                                       std::uint64_t requestGeneration) noexcept {
    return catalogGeneration != 0U && requestGeneration != 0U;
}

[[nodiscard]] bool isKnownEligibilityReason(ApplicationCatalogEligibilityReason reason) noexcept {
    switch (reason) {
    case ApplicationCatalogEligibilityReason::eligibleWithoutTryExec:
    case ApplicationCatalogEligibilityReason::eligibleTryExec:
    case ApplicationCatalogEligibilityReason::excludedTryExecMissing:
    case ApplicationCatalogEligibilityReason::excludedTryExecNotRegularFile:
    case ApplicationCatalogEligibilityReason::excludedTryExecNotExecutable:
        return true;
    }
    return false;
}

} // namespace

Result<ApplicationSearchQuery> parseApplicationSearchQuery(std::string_view text) {
    if (text.size() > maximumApplicationSearchQueryBytes) {
        return queryFailure(ErrorCode::too_large, "The application search query is too large.",
                            "Shorten the query to the documented search limit.");
    }
    switch (validateQueryUtf8(text)) {
    case Utf8Status::invalid:
    case Utf8Status::unsafeControl:
        return queryFailure(ErrorCode::validation_error, "The application search query is invalid.",
                            "Use valid UTF-8 text without unsafe control characters.");
    case Utf8Status::tooManyCodepoints:
        return queryFailure(ErrorCode::too_large,
                            "The application search query has too many code points.",
                            "Shorten the query to the documented search limit.");
    case Utf8Status::valid:
        break;
    }

    std::vector<std::string> tokens;
    std::size_t offset = 0U;
    while (offset < text.size()) {
        while (offset < text.size() &&
               isAsciiWhitespace(static_cast<unsigned char>(text[offset]))) {
            ++offset;
        }
        const auto begin = offset;
        while (offset < text.size() &&
               !isAsciiWhitespace(static_cast<unsigned char>(text[offset]))) {
            ++offset;
        }
        if (begin != offset) {
            if (tokens.size() >= maximumApplicationSearchQueryTokens) {
                return queryFailure(ErrorCode::too_large,
                                    "The application search query has too many tokens.",
                                    "Use fewer whitespace-separated search terms.");
            }
            tokens.push_back(foldAscii(text.substr(begin, offset - begin)));
        }
    }

    std::string phrase;
    for (const auto &token : tokens) {
        if (!phrase.empty()) {
            phrase.push_back(' ');
        }
        phrase.append(token);
    }
    return Result<ApplicationSearchQuery>::success(
        ApplicationSearchQuery{std::move(tokens), std::move(phrase)});
}

struct ApplicationSearchOperation::Impl final {
    Impl(std::shared_ptr<const ApplicationCatalogSnapshot> source, std::uint64_t searchGeneration,
         ApplicationSearchQuery searchQuery, std::size_t maximumResults)
        : catalog(std::move(source)), requestGeneration(searchGeneration),
          query(std::move(searchQuery)), resultLimit(maximumResults) {}

    std::shared_ptr<const ApplicationCatalogSnapshot> catalog;
    std::uint64_t requestGeneration;
    ApplicationSearchQuery query;
    std::size_t resultLimit;
    std::size_t cursor{0U};
    std::vector<MatchScore> matches;
    bool truncated{false};
    bool cancellationObserved{false};
    std::shared_ptr<const ApplicationSearchSnapshot> published;

    void retain(MatchScore score) {
        const auto position = std::lower_bound(matches.begin(), matches.end(), score, scoreLess);
        if (matches.size() < resultLimit) {
            matches.insert(position, std::move(score));
            return;
        }
        truncated = true;
        if (position != matches.end()) {
            matches.insert(position, std::move(score));
            matches.pop_back();
        }
    }

    [[nodiscard]] std::shared_ptr<const ApplicationSearchSnapshot> publish() {
        std::vector<ApplicationSearchResult> results;
        results.reserve(matches.size());
        for (const auto &match : matches) {
            results.push_back({match.entryIndex});
        }

        ApplicationSearchViewState state = ApplicationSearchViewState::loading;
        const bool examinedCurrentCatalog = cursor == catalog->eligibleEntryIndices.size();
        if (catalog->complete && examinedCurrentCatalog) {
            if (catalog->eligibleEntryIndices.empty()) {
                state = ApplicationSearchViewState::emptyCatalog;
            } else if (results.empty()) {
                state = ApplicationSearchViewState::noResults;
            } else {
                state = ApplicationSearchViewState::results;
            }
        }
        published = std::make_shared<const ApplicationSearchSnapshot>(ApplicationSearchSnapshot{
            catalog->generation, requestGeneration, state, cursor,
            catalog->eligibleEntryIndices.size(), std::move(results), truncated});
        return published;
    }
};

ApplicationSearchOperation::ApplicationSearchOperation(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

ApplicationSearchOperation::~ApplicationSearchOperation() = default;
ApplicationSearchOperation::ApplicationSearchOperation(ApplicationSearchOperation &&) noexcept =
    default;
ApplicationSearchOperation &
ApplicationSearchOperation::operator=(ApplicationSearchOperation &&) noexcept = default;

Result<std::shared_ptr<const ApplicationSearchSnapshot>>
ApplicationSearchOperation::advance(std::size_t maximumWorkUnits,
                                    const foundation::CancellationToken &cancellation) {
    if (maximumWorkUnits == 0U || maximumWorkUnits > maximumApplicationSearchWorkUnits) {
        return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
            ErrorCode::invalid_argument, "The application search work budget is invalid.",
            "Use a nonzero work budget within the documented search limit.");
    }
    if (implementation_->cancellationObserved || cancellation.isCancellationRequested()) {
        implementation_->cancellationObserved = true;
        return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
            ErrorCode::cancelled, "Application search was cancelled.",
            "Discard the stale request and begin a current search if needed.");
    }

    std::size_t workUnits = 0U;
    while (implementation_->cursor < implementation_->catalog->eligibleEntryIndices.size() &&
           workUnits < maximumWorkUnits) {
        if (cancellation.isCancellationRequested()) {
            implementation_->cancellationObserved = true;
            return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
                ErrorCode::cancelled, "Application search was cancelled.",
                "Discard the stale request and begin a current search if needed.");
        }
        const auto entryIndex =
            implementation_->catalog->eligibleEntryIndices[implementation_->cursor];
        bool cancelled = false;
        auto score = scoreEntry(implementation_->catalog->discovery->entries[entryIndex],
                                entryIndex, implementation_->query, cancellation, cancelled);
        if (cancelled) {
            implementation_->cancellationObserved = true;
            return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
                ErrorCode::cancelled, "Application search was cancelled.",
                "Discard the stale request and begin a current search if needed.");
        }
        if (score) {
            implementation_->retain(std::move(*score));
        }
        ++implementation_->cursor;
        ++workUnits;
    }
    if (cancellation.isCancellationRequested()) {
        implementation_->cancellationObserved = true;
        return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
            ErrorCode::cancelled, "Application search was cancelled.",
            "Discard the stale request and begin a current search if needed.");
    }
    return Result<std::shared_ptr<const ApplicationSearchSnapshot>>::success(
        implementation_->publish());
}

Result<ApplicationSearchOperation>
createApplicationSearch(std::shared_ptr<const ApplicationCatalogSnapshot> catalog,
                        std::uint64_t requestGeneration, ApplicationSearchQuery query,
                        std::size_t resultLimit) {
    if (!catalog) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::invalid_argument, "The application search catalog is unavailable.",
            "Provide one immutable application catalog snapshot.");
    }
    if (!generationsAreValid(catalog->generation, requestGeneration)) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::invalid_argument, "The application search generation is invalid.",
            "Use nonzero catalog and request generations.");
    }
    if (resultLimit == 0U || resultLimit > maximumApplicationSearchResults) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::invalid_argument, "The application search result limit is invalid.",
            "Use a nonzero result limit within the documented search limit.");
    }
    if (!catalog->discovery) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::validation_error, "The application search catalog is invalid.",
            "Rebuild one immutable application catalog snapshot before searching.");
    }
    const auto &discovery = *catalog->discovery;
    if (discovery.entries.size() > maximumDesktopDiscoveryEntries ||
        discovery.visibleEntryIndices.size() > maximumDesktopDiscoveryEntries ||
        discovery.diagnostics.size() > maximumDesktopDiscoveryDiagnostics ||
        catalog->decisions.size() > maximumDesktopDiscoveryEntries ||
        catalog->eligibleEntryIndices.size() > maximumDesktopDiscoveryEntries) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::too_large, "The application search catalog is too large.",
            "Rebuild discovery within its documented entry limit.");
    }

    if (catalog->totalVisibleEntries != discovery.visibleEntryIndices.size() ||
        catalog->examinedEntries != catalog->decisions.size() ||
        catalog->examinedEntries > catalog->totalVisibleEntries ||
        catalog->complete !=
            (discovery.complete && catalog->examinedEntries == catalog->totalVisibleEntries)) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::validation_error, "The application search catalog is invalid.",
            "Rebuild one immutable application catalog snapshot before searching.");
    }

    std::unordered_set<std::size_t> seenIndices;
    std::unordered_set<std::string> seenDesktopFileIds;
    seenIndices.reserve(discovery.visibleEntryIndices.size());
    seenDesktopFileIds.reserve(discovery.entries.size());
    for (const auto &entry : discovery.entries) {
        if (!seenDesktopFileIds.insert(entry.id.value()).second) {
            return operationFailure<ApplicationSearchOperation>(
                ErrorCode::validation_error, "The application search catalog is invalid.",
                "Rebuild one immutable application catalog snapshot before searching.");
        }
    }
    for (const auto index : discovery.visibleEntryIndices) {
        if (index >= discovery.entries.size() || !seenIndices.insert(index).second ||
            !isVisible(discovery.entries[index].visibility)) {
            return operationFailure<ApplicationSearchOperation>(
                ErrorCode::validation_error, "The application search catalog is invalid.",
                "Rebuild one immutable application catalog snapshot before searching.");
        }
    }

    std::vector<std::size_t> expectedEligibleIndices;
    expectedEligibleIndices.reserve(catalog->decisions.size());
    for (std::size_t position = 0U; position < catalog->decisions.size(); ++position) {
        const auto &decision = catalog->decisions[position];
        if (decision.discoveryEntryIndex != discovery.visibleEntryIndices[position] ||
            !isKnownEligibilityReason(decision.reason)) {
            return operationFailure<ApplicationSearchOperation>(
                ErrorCode::validation_error, "The application search catalog is invalid.",
                "Rebuild one immutable application catalog snapshot before searching.");
        }
        if (isCatalogEligible(decision.reason)) {
            expectedEligibleIndices.push_back(decision.discoveryEntryIndex);
        }
    }
    if (catalog->eligibleEntryIndices != expectedEligibleIndices) {
        return operationFailure<ApplicationSearchOperation>(
            ErrorCode::validation_error, "The application search catalog is invalid.",
            "Rebuild one immutable application catalog snapshot before searching.");
    }

    return Result<ApplicationSearchOperation>::success(
        ApplicationSearchOperation{std::make_unique<ApplicationSearchOperation::Impl>(
            std::move(catalog), requestGeneration, std::move(query), resultLimit)});
}

Result<std::shared_ptr<const ApplicationSearchSnapshot>>
makeApplicationSearchErrorSnapshot(std::uint64_t catalogGeneration,
                                   std::uint64_t requestGeneration) {
    if (!generationsAreValid(catalogGeneration, requestGeneration)) {
        return operationFailure<std::shared_ptr<const ApplicationSearchSnapshot>>(
            ErrorCode::invalid_argument, "The application search generation is invalid.",
            "Use nonzero catalog and request generations.");
    }
    return Result<std::shared_ptr<const ApplicationSearchSnapshot>>::success(
        std::make_shared<const ApplicationSearchSnapshot>(
            ApplicationSearchSnapshot{catalogGeneration,
                                      requestGeneration,
                                      ApplicationSearchViewState::error,
                                      0U,
                                      0U,
                                      {},
                                      false}));
}

} // namespace prismdrake::launcher
