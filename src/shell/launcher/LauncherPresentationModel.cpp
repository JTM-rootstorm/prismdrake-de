#include "LauncherPresentationModel.hpp"

#include "DesktopEntryParser.hpp"

#include <QModelIndex>
#include <QStringDecoder>
#include <QThread>

#include <algorithm>
#include <exception>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace prismdrake::shell::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;
using prismdrake::launcher::ApplicationCatalogSnapshot;
using prismdrake::launcher::ApplicationSearchSnapshot;
using prismdrake::launcher::ApplicationSearchViewState;
using prismdrake::launcher::DesktopFileId;
using prismdrake::launcher::DiscoveredDesktopEntry;

class ApplyingSnapshotGuard final {
  public:
    explicit ApplyingSnapshotGuard(bool &applying) noexcept : applying_(&applying) {
        applying = true;
    }
    ~ApplyingSnapshotGuard() {
        if (applying_ != nullptr) {
            *applying_ = false;
        }
    }

    ApplyingSnapshotGuard(const ApplyingSnapshotGuard &) = delete;
    ApplyingSnapshotGuard &operator=(const ApplyingSnapshotGuard &) = delete;

    void finish() noexcept {
        *applying_ = false;
        applying_ = nullptr;
    }

  private:
    bool *applying_;
};

[[nodiscard]] Result<void> invalid(std::string message) {
    return Result<void>::failure(
        {ErrorCode::validation_error, std::move(message),
         "retain the prior launcher view and publish matching bounded catalog/search snapshots"});
}

[[nodiscard]] bool validLiteral(std::string_view value, bool required) {
    if ((required && value.empty()) ||
        value.size() > prismdrake::launcher::maximumDesktopEntryValueBytes) {
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded =
        decoder.decode(QByteArray::fromRawData(value.data(), static_cast<qsizetype>(value.size())));
    if (decoder.hasError()) {
        return false;
    }
    for (const auto character : decoded) {
        const auto codepoint = character.unicode();
        if ((codepoint < 0x20U && codepoint != '\t' && codepoint != '\n') ||
            (codepoint >= 0x7fU && codepoint <= 0x9fU)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QString literal(const std::optional<std::string> &value) {
    if (!value) {
        return {};
    }
    return QString::fromUtf8(value->data(), static_cast<qsizetype>(value->size()));
}

[[nodiscard]] Result<void>
validateCatalog(const std::shared_ptr<const ApplicationCatalogSnapshot> &catalog,
                std::uint64_t requestGeneration) {
    if (!catalog->complete || !catalog->discovery || !catalog->discovery->complete) {
        return invalid("launcher presentation catalog is incomplete");
    }
    auto query = prismdrake::launcher::parseApplicationSearchQuery({});
    if (!query) {
        return Result<void>::failure(query.error());
    }
    auto validated = prismdrake::launcher::createApplicationSearch(
        catalog, requestGeneration, std::move(query).value(),
        prismdrake::launcher::maximumApplicationSearchResults);
    if (!validated) {
        return Result<void>::failure(validated.error());
    }
    for (const auto index : catalog->eligibleEntryIndices) {
        const auto &entry = catalog->discovery->entries[index].entry;
        if (!entry.name || !entry.exec || !validLiteral(*entry.name, true) ||
            !validLiteral(entry.genericName.value_or(""), false) ||
            !validLiteral(entry.comment.value_or(""), false) ||
            !validLiteral(entry.icon.value_or(""), false)) {
            return invalid("launcher presentation entry has invalid literal fields");
        }
    }
    return Result<void>::success();
}

[[nodiscard]] std::optional<LauncherPresentationModel::ViewState>
mappedViewState(ApplicationSearchViewState state) noexcept {
    using ViewState = LauncherPresentationModel::ViewState;
    switch (state) {
    case ApplicationSearchViewState::loading:
        return ViewState::loading;
    case ApplicationSearchViewState::results:
        return ViewState::results;
    case ApplicationSearchViewState::emptyCatalog:
        return ViewState::emptyCatalog;
    case ApplicationSearchViewState::noResults:
        return ViewState::noResults;
    case ApplicationSearchViewState::error:
        return ViewState::error;
    }
    return std::nullopt;
}

[[nodiscard]] Result<void> validateSearch(const ApplicationCatalogSnapshot &catalog,
                                          const ApplicationSearchSnapshot &search) {
    if (search.catalogGeneration != catalog.generation || search.requestGeneration == 0U ||
        search.results.size() > prismdrake::launcher::maximumApplicationSearchResults ||
        !mappedViewState(search.state)) {
        return invalid("launcher presentation search generations or bounds are invalid");
    }
    const auto total = catalog.eligibleEntryIndices.size();
    std::unordered_set<std::size_t> eligible(catalog.eligibleEntryIndices.begin(),
                                             catalog.eligibleEntryIndices.end());
    std::unordered_set<std::size_t> seen;
    for (const auto result : search.results) {
        if (!eligible.contains(result.discoveryEntryIndex) ||
            !seen.insert(result.discoveryEntryIndex).second) {
            return invalid("launcher presentation search contains an invalid result identity");
        }
    }

    const bool commonShape = search.examinedApplications <= search.totalApplications &&
                             search.results.size() <= search.examinedApplications;
    bool stateShape = false;
    switch (search.state) {
    case ApplicationSearchViewState::loading:
        stateShape = commonShape && search.totalApplications == total &&
                     search.examinedApplications < search.totalApplications;
        break;
    case ApplicationSearchViewState::results:
        stateShape = commonShape && search.totalApplications == total &&
                     search.examinedApplications == total && !search.results.empty();
        break;
    case ApplicationSearchViewState::emptyCatalog:
        stateShape = total == 0U && search.totalApplications == 0U &&
                     search.examinedApplications == 0U && search.results.empty() &&
                     !search.truncated;
        break;
    case ApplicationSearchViewState::noResults:
        stateShape = total > 0U && search.totalApplications == total &&
                     search.examinedApplications == total && search.results.empty() &&
                     !search.truncated;
        break;
    case ApplicationSearchViewState::error:
        stateShape = search.totalApplications == 0U && search.examinedApplications == 0U &&
                     search.results.empty() && !search.truncated;
        break;
    }
    return stateShape ? Result<void>::success()
                      : invalid("launcher presentation search state shape is invalid");
}

} // namespace

LauncherResultPresentation::LauncherResultPresentation(const DiscoveredDesktopEntry &entry,
                                                       LauncherPresentationModel *owner)
    : QObject(owner), desktop_file_id_(entry.id), owner_(owner), name_(literal(entry.entry.name)),
      generic_name_(literal(entry.entry.genericName)), comment_(literal(entry.entry.comment)),
      icon_(literal(entry.entry.icon)), terminal_required_(entry.entry.terminal) {}

bool LauncherResultPresentation::matches(const DiscoveredDesktopEntry &entry) const noexcept {
    return desktop_file_id_ == entry.id && name_ == literal(entry.entry.name) &&
           generic_name_ == literal(entry.entry.genericName) &&
           comment_ == literal(entry.entry.comment) && icon_ == literal(entry.entry.icon) &&
           terminal_required_ == entry.entry.terminal;
}

bool LauncherResultPresentation::requestLaunch() { return owner_->requestLaunch(*this); }

LauncherPresentationModel::LauncherPresentationModel(QObject *parent) : QAbstractListModel(parent) {
    results_.reserve(prismdrake::launcher::maximumApplicationSearchResults);
}

int LauncherPresentationModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

QVariant LauncherPresentationModel::data(const QModelIndex &index, int role) const {
    auto *result = resultAt(index.row());
    if (result == nullptr || index.parent().isValid() || role != Role::resultObject) {
        return {};
    }
    return QVariant::fromValue(static_cast<QObject *>(result));
}

QHash<int, QByteArray> LauncherPresentationModel::roleNames() const {
    return {{Role::resultObject, QByteArrayLiteral("result")}};
}

foundation::Result<void>
LauncherPresentationModel::applySnapshot(std::shared_ptr<const ApplicationCatalogSnapshot> catalog,
                                         std::shared_ptr<const ApplicationSearchSnapshot> search) {
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "launcher presentation was called from a non-owner thread",
             "queue matching complete snapshots to the model's QObject thread"});
    }
    if (applying_snapshot_) {
        return Result<void>::failure(
            {ErrorCode::cancelled, "launcher presentation application is already in progress",
             "queue the newer publication until publicationApplied is emitted"});
    }
    if (!catalog || !search) {
        return Result<void>::failure(
            {ErrorCode::invalid_argument, "launcher presentation snapshots are absent",
             "retain the prior view until matching complete snapshots are available"});
    }
    auto validCatalog = validateCatalog(catalog, search->requestGeneration);
    if (!validCatalog) {
        return validCatalog;
    }
    auto validSearch = validateSearch(*catalog, *search);
    if (!validSearch) {
        return validSearch;
    }
    if (search_) {
        const bool samePublication = search->catalogGeneration == search_->catalogGeneration &&
                                     search->requestGeneration == search_->requestGeneration;
        if (samePublication) {
            if (catalog == catalog_ && search == search_) {
                return Result<void>::success();
            }
            const bool validProgress = catalog == catalog_ &&
                                       search_->state == ApplicationSearchViewState::loading &&
                                       search->examinedApplications > search_->examinedApplications;
            if (!validProgress) {
                return Result<void>::failure(
                    {ErrorCode::validation_error,
                     "launcher generation has conflicting presentation content",
                     "retain the prior view and publish changed content with a new request"});
            }
        } else if (search->catalogGeneration < search_->catalogGeneration ||
                   (search->catalogGeneration == search_->catalogGeneration &&
                    search->requestGeneration < search_->requestGeneration)) {
            return Result<void>::failure({ErrorCode::cancelled,
                                          "launcher presentation snapshot is stale",
                                          "retain the newer complete launcher publication"});
        }
    }

    std::vector<std::unique_ptr<LauncherResultPresentation>> replacements;
    std::vector<std::unique_ptr<LauncherResultPresentation>> retiredPresentations;
    try {
        replacements.resize(search->results.size());
        retiredPresentations.reserve(results_.size());
        for (std::size_t index = 0U; index < search->results.size(); ++index) {
            const auto entryIndex = search->results[index].discoveryEntryIndex;
            const auto &entry = catalog->discovery->entries[entryIndex];
            const auto prior = std::ranges::find_if(results_, [&entry](const auto &result) {
                return result->desktop_file_id_ == entry.id;
            });
            if (prior != results_.end() && (*prior)->matches(entry)) {
                continue;
            }
            replacements[index] = std::unique_ptr<LauncherResultPresentation>(
                new LauncherResultPresentation(entry, this));
        }
    } catch (const std::exception &) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "launcher presentation could not allocate bounded rows",
                                      "retain the prior view and reduce the bounded result set"});
    }

    ApplyingSnapshotGuard applyingGuard{applying_snapshot_};
    emit publicationReconciliationStarted();

    // Prefer an in-place row substitution when neither the old identity nor the new identity is
    // retained elsewhere. This is a data change, not a remove-plus-insert pair, and lets views
    // keep keyboard focus on the bounded fallback row without pinning a removed delegate.
    const auto commonRows = std::min(results_.size(), search->results.size());
    for (std::size_t row = 0U; row < commonRows; ++row) {
        const auto newEntryIndex = search->results[row].discoveryEntryIndex;
        const auto &newEntry = catalog->discovery->entries[newEntryIndex];
        const bool oldRetained = std::ranges::any_of(search->results, [&](const auto &result) {
            return catalog->discovery->entries[result.discoveryEntryIndex].id ==
                   results_[row]->desktop_file_id_;
        });
        const bool newAlreadyPresent = std::ranges::any_of(
            results_, [&](const auto &result) { return result->desktop_file_id_ == newEntry.id; });
        if (!oldRetained && !newAlreadyPresent && replacements[row]) {
            auto retired = std::move(results_[row]);
            results_[row] = std::move(replacements[row]);
            emit dataChanged(index(static_cast<int>(row)), index(static_cast<int>(row)),
                             {Role::resultObject});
            retiredPresentations.push_back(std::move(retired));
        }
    }

    for (std::size_t index = results_.size(); index > 0U; --index) {
        const auto row = index - 1U;
        const auto retained = std::ranges::any_of(search->results, [&](const auto &result) {
            return catalog->discovery->entries[result.discoveryEntryIndex].id ==
                   results_[row]->desktop_file_id_;
        });
        if (!retained) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            auto retired = std::move(results_[row]);
            results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(row));
            endRemoveRows();
            retiredPresentations.push_back(std::move(retired));
        }
    }

    for (std::size_t index = 0U; index < search->results.size(); ++index) {
        const auto entryIndex = search->results[index].discoveryEntryIndex;
        const auto &entry = catalog->discovery->entries[entryIndex];
        auto current = std::ranges::find_if(results_, [&entry](const auto &result) {
            return result->desktop_file_id_ == entry.id;
        });
        if (current == results_.end()) {
            beginInsertRows({}, static_cast<int>(index), static_cast<int>(index));
            results_.insert(results_.begin() + static_cast<std::ptrdiff_t>(index),
                            std::move(replacements[index]));
            endInsertRows();
            continue;
        }
        auto currentIndex = static_cast<std::size_t>(std::distance(results_.begin(), current));
        if (currentIndex != index) {
            beginMoveRows({}, static_cast<int>(currentIndex), static_cast<int>(currentIndex), {},
                          static_cast<int>(index));
            auto moved = std::move(results_[currentIndex]);
            results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(currentIndex));
            results_.insert(results_.begin() + static_cast<std::ptrdiff_t>(index),
                            std::move(moved));
            endMoveRows();
            currentIndex = index;
        }
        if (replacements[index]) {
            auto retired = std::move(results_[currentIndex]);
            results_[currentIndex] = std::move(replacements[index]);
            emit dataChanged(this->index(static_cast<int>(currentIndex)),
                             this->index(static_cast<int>(currentIndex)), {Role::resultObject});
            retiredPresentations.push_back(std::move(retired));
        }
    }

    catalog_ = std::move(catalog);
    search_ = std::move(search);
    view_state_ = *mappedViewState(search_->state);
    truncated_ = search_->truncated;
    const bool refreshObjectRoles = !retiredPresentations.empty() && !results_.empty();
    // QObject-backed roles remain valid through every standard begin/end model notification.
    // Retire them only after synchronous structural reconciliation, then refresh the bounded
    // current role range so views cannot retain a null or stale cached QObject before the final
    // coherent publication signal.
    retiredPresentations.clear();
    if (refreshObjectRoles) {
        emit dataChanged(index(0), index(rowCount() - 1), {Role::resultObject});
    }
    applyingGuard.finish();
    emit publicationApplied();
    return Result<void>::success();
}

QString LauncherPresentationModel::stateId() const {
    switch (view_state_) {
    case ViewState::loading:
        return QStringLiteral("loading");
    case ViewState::results:
        return QStringLiteral("results");
    case ViewState::emptyCatalog:
        return QStringLiteral("emptyCatalog");
    case ViewState::noResults:
        return QStringLiteral("noResults");
    case ViewState::error:
        return QStringLiteral("error");
    }
    return {};
}

QString LauncherPresentationModel::stateLabel() const {
    switch (view_state_) {
    case ViewState::loading:
        return tr("Loading applications");
    case ViewState::results:
        return tr("Applications");
    case ViewState::emptyCatalog:
        return tr("No applications are available");
    case ViewState::noResults:
        return tr("No matching applications");
    case ViewState::error:
        return tr("Application search is unavailable");
    }
    return {};
}

LauncherResultPresentation *LauncherPresentationModel::resultAt(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= results_.size()) {
        return nullptr;
    }
    return results_[static_cast<std::size_t>(row)].get();
}

bool LauncherPresentationModel::containsCurrentIdentity(
    const DesktopFileId &desktopFileId) const noexcept {
    if (!catalog_ || !search_) {
        return false;
    }
    return std::ranges::any_of(search_->results, [&](const auto &result) {
        return catalog_->discovery->entries[result.discoveryEntryIndex].id == desktopFileId;
    });
}

bool LauncherPresentationModel::requestLaunch(const LauncherResultPresentation &result) {
    const bool isActivePresentation = std::ranges::any_of(
        results_, [&result](const auto &candidate) { return candidate.get() == &result; });
    if (QThread::currentThread() != thread() || applying_snapshot_ || !isActivePresentation ||
        !containsCurrentIdentity(result.desktop_file_id_)) {
        return false;
    }
    emit launchRequested(ApplicationLaunchIntent{result.desktop_file_id_, catalog_->generation,
                                                 search_->requestGeneration});
    return true;
}

} // namespace prismdrake::shell::launcher
