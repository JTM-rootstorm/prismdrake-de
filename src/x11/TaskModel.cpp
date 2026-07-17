#include "TaskModel.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <utility>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;
using foundation::Result;
using SnapshotResult = Result<std::shared_ptr<const TaskModelSnapshot>>;

[[nodiscard]] SnapshotResult invalidObservation() {
    return SnapshotResult::failure(
        {ErrorCode::validation_error, "The decoded X11 task observation is invalid.",
         "Discard the complete observation and retain the previous immutable task snapshot."});
}

[[nodiscard]] SnapshotResult oversizedObservation() {
    return SnapshotResult::failure(
        {ErrorCode::too_large, "The decoded X11 task observation exceeds its bounds.",
         "Discard the complete observation and retain the previous immutable task snapshot."});
}

[[nodiscard]] SnapshotResult exhaustedIdentifiers() {
    return SnapshotResult::failure(
        {ErrorCode::validation_error, "The X11 task model identifier space is exhausted.",
         "Restart the development shell and rebuild state from the authoritative window manager."});
}

[[nodiscard]] bool validWindowType(WindowType type) noexcept {
    switch (type) {
    case WindowType::normal:
    case WindowType::dialog:
    case WindowType::utility:
    case WindowType::toolbar:
    case WindowType::menu:
    case WindowType::splash:
    case WindowType::dropdownMenu:
    case WindowType::popupMenu:
    case WindowType::tooltip:
    case WindowType::notification:
    case WindowType::combo:
    case WindowType::dragAndDrop:
    case WindowType::dock:
    case WindowType::desktop:
        return true;
    }
    return false;
}

[[nodiscard]] bool validIdentitySource(ApplicationIdentitySource source) noexcept {
    switch (source) {
    case ApplicationIdentitySource::wmClass:
    case ApplicationIdentitySource::genericUnknown:
        return true;
    }
    return false;
}

[[nodiscard]] bool excluded(const WindowMetadata &window) noexcept {
    return window.skipTaskbar || window.type == WindowType::dropdownMenu ||
           window.type == WindowType::popupMenu || window.type == WindowType::tooltip ||
           window.type == WindowType::notification || window.type == WindowType::combo ||
           window.type == WindowType::dragAndDrop || window.type == WindowType::dock ||
           window.type == WindowType::desktop;
}

struct CandidateRecord final {
    WindowId window;
    TaskLifetimeId lifetime;
    const DecodedTaskObservation *decoded;
};

} // namespace

Result<WindowIncarnationId> WindowIncarnationId::fromObserved(Value value) {
    if (value == 0U) {
        return Result<WindowIncarnationId>::failure(
            {ErrorCode::invalid_argument, "The X11 window incarnation identifier is invalid.",
             "Use a nonzero decoder-assigned identifier for one observed window lifetime."});
    }
    return Result<WindowIncarnationId>::success(WindowIncarnationId{value});
}

TaskRecord::TaskRecord(WindowId window, WindowIncarnationId incarnation, TaskLifetimeId lifetime,
                       std::string title, std::string applicationId,
                       ApplicationIdentitySource identitySource, std::string fallbackIconName,
                       bool active, bool hidden, bool urgent,
                       std::optional<std::uint32_t> workspace, bool onAllWorkspaces,
                       WindowType type, std::optional<TaskLifetimeId> transientParent, bool modal,
                       TaskModelGeneration generation)
    : window_(window), incarnation_(incarnation), lifetime_(lifetime), title_(std::move(title)),
      application_id_(std::move(applicationId)), application_identity_source_(identitySource),
      fallback_icon_name_(std::move(fallbackIconName)), active_(active), hidden_(hidden),
      urgent_(urgent), workspace_(workspace), on_all_workspaces_(onAllWorkspaces), type_(type),
      transient_parent_(transientParent), modal_(modal), last_observed_generation_(generation) {}

TaskModelSnapshot::TaskModelSnapshot(TaskModelGeneration generation, std::vector<TaskRecord> tasks,
                                     std::vector<AuthoritativeTaskClient> authoritativeClients)
    : generation_(generation), tasks_(std::move(tasks)),
      authoritative_clients_(std::move(authoritativeClients)) {}

bool TaskModelSnapshot::authoritativelyContains(WindowId window,
                                                WindowIncarnationId incarnation) const noexcept {
    return std::ranges::find(authoritative_clients_,
                             AuthoritativeTaskClient{window, incarnation}) !=
           authoritative_clients_.end();
}

SnapshotResult TaskModel::publish(const TaskModelObservation &observation) {
    if (observation.authoritative.clientList().size() > maximumTaskWindows ||
        observation.windows.size() > maximumTaskWindows) {
        return oversizedObservation();
    }
    if (next_generation_ == 0U) {
        return exhaustedIdentifiers();
    }

    std::map<WindowId::Value, const DecodedTaskObservation *> decodedByWindow;
    std::set<WindowIncarnationId::Value> seenIncarnations;
    for (const auto &decoded : observation.windows) {
        if (!observation.authoritative.contains(decoded.window) ||
            !decodedByWindow.emplace(decoded.window.value(), &decoded).second ||
            !seenIncarnations.insert(decoded.incarnation.value()).second) {
            return invalidObservation();
        }
        if (!decoded.metadata) {
            if (!decoded.fallbackIconName.empty()) {
                return invalidObservation();
            }
            continue;
        }
        const auto &metadata = decoded.metadata.value();
        if (metadata.displayTitle.size() > maximumWindowTitleBytes ||
            metadata.identity.groupingKey.size() > maximumTaskApplicationIdBytes ||
            decoded.fallbackIconName.size() > maximumTaskFallbackIconNameBytes) {
            return oversizedObservation();
        }
        if (metadata.window != decoded.window || !validWindowType(metadata.type) ||
            !validIdentitySource(metadata.identity.source) ||
            metadata.identity.groupingKey.empty() || decoded.fallbackIconName.empty() ||
            (metadata.transientFor && metadata.transientFor.value() == metadata.window)) {
            return invalidObservation();
        }
    }
    if (decodedByWindow.size() != observation.authoritative.clientList().size()) {
        return invalidObservation();
    }

    std::map<WindowId::Value, TrackedWindow> observedTracked;
    std::vector<CandidateRecord> candidates;
    candidates.reserve(observation.windows.size());
    auto candidateNextLifetime = next_lifetime_;

    for (const auto window : observation.authoritative.clientList()) {
        const auto decodedEntry = decodedByWindow.find(window.value());
        if (decodedEntry == decodedByWindow.end()) {
            continue;
        }
        const auto &decoded = *decodedEntry->second;
        std::optional<TaskLifetimeId> lifetime;
        const auto existing = tracked_.find(window.value());
        if (existing != tracked_.end() && existing->second.incarnation == decoded.incarnation) {
            lifetime = existing->second.lifetime;
        }
        if (decoded.metadata && !excluded(decoded.metadata.value()) && !lifetime) {
            if (candidateNextLifetime == 0U) {
                return exhaustedIdentifiers();
            }
            lifetime = TaskLifetimeId{candidateNextLifetime};
            candidateNextLifetime =
                candidateNextLifetime == std::numeric_limits<std::uint64_t>::max()
                    ? 0U
                    : candidateNextLifetime + 1U;
        }
        observedTracked.emplace(window.value(), TrackedWindow{decoded.incarnation, lifetime});
        if (decoded.metadata && !excluded(decoded.metadata.value())) {
            candidates.push_back(CandidateRecord{window, lifetime.value(), &decoded});
        }
    }

    const TaskModelGeneration generation{next_generation_};
    std::map<WindowId::Value, TaskLifetimeId> includedLifetimes;
    for (const auto &candidate : candidates) {
        includedLifetimes.emplace(candidate.window.value(), candidate.lifetime);
    }

    std::vector<TaskRecord> records;
    records.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        const auto &decoded = *candidate.decoded;
        const auto &metadata = decoded.metadata.value();
        std::optional<TaskLifetimeId> transientParent;
        if (metadata.transientFor) {
            const auto parent = includedLifetimes.find(metadata.transientFor->value());
            if (parent != includedLifetimes.end()) {
                transientParent = parent->second;
            }
        }
        records.push_back(TaskRecord{
            candidate.window, decoded.incarnation, candidate.lifetime, metadata.displayTitle,
            metadata.identity.groupingKey, metadata.identity.source, decoded.fallbackIconName,
            observation.authoritative.activeWindow() &&
                observation.authoritative.activeWindow().value() == candidate.window,
            metadata.minimized, metadata.urgent, metadata.workspace, metadata.onAllWorkspaces,
            metadata.type, transientParent, metadata.modal, generation});
    }

    std::vector<AuthoritativeTaskClient> authoritativeClients;
    authoritativeClients.reserve(observation.windows.size());
    for (const auto window : observation.authoritative.clientList()) {
        const auto decoded = decodedByWindow.find(window.value());
        authoritativeClients.push_back({window, decoded->second->incarnation});
    }

    std::shared_ptr<const TaskModelSnapshot> snapshot{
        new TaskModelSnapshot{generation, std::move(records), std::move(authoritativeClients)}};
    tracked_ = std::move(observedTracked);
    next_lifetime_ = candidateNextLifetime;
    next_generation_ =
        next_generation_ == std::numeric_limits<std::uint64_t>::max() ? 0U : next_generation_ + 1U;
    current_ = snapshot;
    return SnapshotResult::success(std::move(snapshot));
}

} // namespace prismdrake::x11
