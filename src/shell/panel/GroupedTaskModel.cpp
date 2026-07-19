#include "GroupedTaskModel.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace prismdrake::shell::panel {
namespace {

using foundation::ErrorCode;
using foundation::Result;

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure({code, std::move(message), std::move(recovery)});
}

struct CatalogEntry final {
    const launcher::DesktopFileId *id;
    const launcher::DesktopEntry *entry;
};

struct TaskAssociation final {
    const x11::TaskRecord *task;
    const launcher::ApplicationAssociation *association;
};

[[nodiscard]] bool validGrouping(config::GroupingMode mode) noexcept {
    switch (mode) {
    case config::GroupingMode::always:
    case config::GroupingMode::when_full:
    case config::GroupingMode::never:
        return true;
    }
    return false;
}

[[nodiscard]] std::string taskKey(x11::TaskLifetimeId lifetime) {
    return "task:" + std::to_string(lifetime.value());
}

[[nodiscard]] std::string applicationKey(const launcher::ApplicationId &application) {
    switch (application.kind()) {
    case launcher::ApplicationIdKind::desktopEntry:
        return "desktop:" + application.value();
    case launcher::ApplicationIdKind::wmClassFallback:
        return "fallback:" + application.value();
    case launcher::ApplicationIdKind::unknown:
        break;
    }
    return {};
}

[[nodiscard]] std::optional<CatalogEntry>
findCatalogEntry(const launcher::ApplicationCatalogSnapshot &catalog, std::string_view id) {
    for (const std::size_t discoveryIndex : catalog.eligibleEntryIndices) {
        const auto &candidate = catalog.discovery->entries[discoveryIndex];
        if (candidate.id.value() == id) {
            return CatalogEntry{&candidate.id, &candidate.entry};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string displayName(const CatalogEntry &entry, const x11::TaskRecord *task) {
    if (entry.entry->name && !entry.entry->name->empty()) {
        return *entry.entry->name;
    }
    return task != nullptr ? task->title() : entry.id->value();
}

[[nodiscard]] std::string iconName(const CatalogEntry &entry, const x11::TaskRecord *task) {
    if (entry.entry->icon && !entry.entry->icon->empty()) {
        return *entry.entry->icon;
    }
    return task != nullptr ? task->fallbackIconName() : "application-x-executable";
}

[[nodiscard]] PanelTaskGroup emptyDesktopGroup(const launcher::DesktopFileId &id,
                                               const std::optional<CatalogEntry> &catalog,
                                               bool pinned) {
    return {PanelTaskGroupKind::desktopEntry,
            "desktop:" + id.value(),
            id,
            catalog ? displayName(*catalog, nullptr) : id.value(),
            catalog ? iconName(*catalog, nullptr) : "application-x-executable",
            pinned,
            catalog.has_value(),
            false,
            false,
            false,
            false,
            catalog ? PanelTaskPrimaryAction::launch : PanelTaskPrimaryAction::unavailable,
            {}};
}

void addMember(PanelTaskGroup &group, const x11::TaskRecord &task) {
    group.members.push_back(
        {task.lifetime(), task.title(), task.active(), task.hidden(), task.urgent(), task.modal()});
    group.active = group.active || task.active();
    group.urgent = group.urgent || task.urgent();
    group.modal = group.modal || task.modal();
    group.minimized = std::ranges::all_of(
        group.members, [](const PanelTaskMember &member) { return member.minimized; });
    group.primaryAction = group.members.size() == 1U ? PanelTaskPrimaryAction::activateOrMinimize
                                                     : PanelTaskPrimaryAction::openGroup;
}

[[nodiscard]] Result<std::vector<TaskAssociation>> validateInput(const GroupedTaskInput &input) {
    if (!input.tasks || !input.associations || !input.catalog || !input.pins ||
        input.settingsGeneration == 0U || input.widthGeneration == 0U ||
        input.associations->generation == 0U || input.catalog->generation == 0U ||
        input.pins->generation == 0U || !input.catalog->complete || !input.catalog->discovery ||
        !input.catalog->discovery->complete || !validGrouping(input.grouping) ||
        !std::isfinite(input.availableTaskStripWidth) || input.availableTaskStripWidth < 0.0 ||
        !std::isfinite(input.minimumTargetSize) || input.minimumTargetSize <= 0.0 ||
        input.minimumTargetSize > 4096.0 || !std::isfinite(input.itemSpacing) ||
        input.itemSpacing < 0.0 || input.itemSpacing > 1024.0 ||
        input.tasks->tasks().size() > x11::maximumTaskWindows ||
        input.associations->associations.size() != input.tasks->tasks().size() ||
        input.pins->pins.size() > maximumPinCount) {
        return failure<std::vector<TaskAssociation>>(
            ErrorCode::validation_error, "The grouped task publication input is invalid.",
            "Retain the prior complete panel generation.");
    }

    std::set<std::string> catalogIds;
    for (const std::size_t index : input.catalog->eligibleEntryIndices) {
        if (index >= input.catalog->discovery->entries.size() ||
            !catalogIds.insert(input.catalog->discovery->entries[index].id.value()).second) {
            return failure<std::vector<TaskAssociation>>(
                ErrorCode::validation_error, "The grouped task catalog is malformed.",
                "Retain the prior complete panel generation.");
        }
    }
    std::set<std::string> pinIds;
    for (const auto &pin : input.pins->pins) {
        if (!pinIds.insert(pin.value()).second) {
            return failure<std::vector<TaskAssociation>>(
                ErrorCode::validation_error, "The grouped task pin set is malformed.",
                "Retain the prior complete panel generation.");
        }
    }

    std::vector<TaskAssociation> joined;
    joined.reserve(input.tasks->tasks().size());
    std::set<std::pair<std::uint32_t, std::uint64_t>> usedAssociations;
    for (const auto &task : input.tasks->tasks()) {
        const auto found = std::ranges::find_if(
            input.associations->associations, [&task](const auto &association) {
                return association.window.xid() == task.window().value() &&
                       association.window.incarnation() == task.incarnation().value();
            });
        if (found == input.associations->associations.end() ||
            !usedAssociations.emplace(found->window.xid(), found->window.incarnation()).second) {
            return failure<std::vector<TaskAssociation>>(
                ErrorCode::validation_error, "The grouped task association set is incoherent.",
                "Retain the prior complete panel generation.");
        }
        if (found->applicationId.kind() == launcher::ApplicationIdKind::desktopEntry &&
            !catalogIds.contains(found->applicationId.value())) {
            return failure<std::vector<TaskAssociation>>(
                ErrorCode::validation_error, "A grouped task application is absent.",
                "Publish associations from the current complete application catalog.");
        }
        joined.push_back({&task, &*found});
    }
    return Result<std::vector<TaskAssociation>>::success(std::move(joined));
}

[[nodiscard]] std::size_t ungroupedCount(const std::vector<TaskAssociation> &tasks,
                                         const PinStateSnapshot &pins) {
    std::size_t missingPins = 0U;
    for (const auto &pin : pins.pins) {
        const bool running = std::ranges::any_of(tasks, [&pin](const TaskAssociation &task) {
            return task.association->applicationId.kind() ==
                       launcher::ApplicationIdKind::desktopEntry &&
                   task.association->applicationId.value() == pin.value();
        });
        missingPins += running ? 0U : 1U;
    }
    return tasks.size() + missingPins;
}

[[nodiscard]] config::GroupingMode effectiveGrouping(const GroupedTaskInput &input,
                                                     const std::vector<TaskAssociation> &tasks) {
    if (input.grouping != config::GroupingMode::when_full) {
        return input.grouping;
    }
    const auto count = ungroupedCount(tasks, *input.pins);
    if (count == 0U) {
        return config::GroupingMode::never;
    }
    const long double required = static_cast<long double>(count) * input.minimumTargetSize +
                                 static_cast<long double>(count - 1U) * input.itemSpacing;
    return required <= static_cast<long double>(input.availableTaskStripWidth)
               ? config::GroupingMode::never
               : config::GroupingMode::always;
}

[[nodiscard]] PanelTaskGroup taskGroup(const TaskAssociation &joined,
                                       const launcher::ApplicationCatalogSnapshot &catalog,
                                       bool pinned, bool groupByApplication) {
    const auto &task = *joined.task;
    const auto &application = joined.association->applicationId;
    std::optional<CatalogEntry> catalogEntry;
    std::optional<launcher::DesktopFileId> desktopFileId;
    PanelTaskGroupKind kind = PanelTaskGroupKind::unknown;
    std::string key = taskKey(task.lifetime());
    if (application.kind() == launcher::ApplicationIdKind::desktopEntry) {
        catalogEntry = findCatalogEntry(catalog, application.value());
        if (catalogEntry) {
            desktopFileId = *catalogEntry->id;
        }
        kind = PanelTaskGroupKind::desktopEntry;
        if (groupByApplication) {
            key = applicationKey(application);
        }
    } else if (application.kind() == launcher::ApplicationIdKind::wmClassFallback) {
        kind = PanelTaskGroupKind::wmClassFallback;
        if (groupByApplication) {
            key = applicationKey(application);
        }
    }
    PanelTaskGroup group{kind,
                         std::move(key),
                         std::move(desktopFileId),
                         catalogEntry ? displayName(*catalogEntry, &task) : task.title(),
                         catalogEntry ? iconName(*catalogEntry, &task) : task.fallbackIconName(),
                         pinned,
                         catalogEntry.has_value(),
                         false,
                         false,
                         false,
                         false,
                         PanelTaskPrimaryAction::unavailable,
                         {}};
    addMember(group, task);
    return group;
}

void appendAlwaysGrouped(std::vector<PanelTaskGroup> &groups,
                         const std::vector<TaskAssociation> &tasks,
                         const launcher::ApplicationCatalogSnapshot &catalog,
                         const PinStateSnapshot &pins) {
    for (const auto &pin : pins.pins) {
        groups.push_back(emptyDesktopGroup(pin, findCatalogEntry(catalog, pin.value()), true));
    }
    for (const auto &joined : tasks) {
        const auto &application = joined.association->applicationId;
        const auto pin =
            application.kind() == launcher::ApplicationIdKind::desktopEntry
                ? std::ranges::find(pins.pins, application.value(), &launcher::DesktopFileId::value)
                : pins.pins.end();
        const bool pinned = pin != pins.pins.end();
        const std::string key = application.kind() == launcher::ApplicationIdKind::unknown
                                    ? taskKey(joined.task->lifetime())
                                    : applicationKey(application);
        const auto existing = std::ranges::find(groups, key, &PanelTaskGroup::internalKey);
        if (existing == groups.end()) {
            groups.push_back(taskGroup(joined, catalog, pinned, true));
        } else {
            if (existing->members.empty()) {
                existing->displayName = taskGroup(joined, catalog, pinned, true).displayName;
                existing->iconName = taskGroup(joined, catalog, pinned, true).iconName;
            }
            addMember(*existing, *joined.task);
        }
    }
}

void appendUngrouped(std::vector<PanelTaskGroup> &groups, const std::vector<TaskAssociation> &tasks,
                     const launcher::ApplicationCatalogSnapshot &catalog,
                     const PinStateSnapshot &pins) {
    std::vector<bool> consumed(tasks.size(), false);
    for (const auto &pin : pins.pins) {
        bool found = false;
        for (std::size_t index = 0U; index < tasks.size(); ++index) {
            const auto &application = tasks[index].association->applicationId;
            if (application.kind() == launcher::ApplicationIdKind::desktopEntry &&
                application.value() == pin.value()) {
                groups.push_back(taskGroup(tasks[index], catalog, true, false));
                consumed[index] = true;
                found = true;
            }
        }
        if (!found) {
            groups.push_back(emptyDesktopGroup(pin, findCatalogEntry(catalog, pin.value()), true));
        }
    }
    for (std::size_t index = 0U; index < tasks.size(); ++index) {
        if (!consumed[index]) {
            groups.push_back(taskGroup(tasks[index], catalog, false, false));
        }
    }
}

} // namespace

Result<std::shared_ptr<const GroupedTaskSnapshot>>
buildGroupedTasks(const GroupedTaskInput &input) {
    auto joined = validateInput(input);
    if (!joined) {
        return Result<std::shared_ptr<const GroupedTaskSnapshot>>::failure(joined.error());
    }
    const auto effective = effectiveGrouping(input, joined.value());
    std::vector<PanelTaskGroup> groups;
    try {
        groups.reserve(joined.value().size() + input.pins->pins.size());
        if (effective == config::GroupingMode::always) {
            appendAlwaysGrouped(groups, joined.value(), *input.catalog, *input.pins);
        } else {
            appendUngrouped(groups, joined.value(), *input.catalog, *input.pins);
        }
        auto snapshot = std::make_shared<const GroupedTaskSnapshot>(GroupedTaskSnapshot{
            input.tasks->generation(), input.associations->generation, input.catalog->generation,
            input.pins->generation, input.settingsGeneration, input.widthGeneration, input.grouping,
            effective, std::move(groups)});
        return Result<std::shared_ptr<const GroupedTaskSnapshot>>::success(std::move(snapshot));
    } catch (const std::exception &) {
        return failure<std::shared_ptr<const GroupedTaskSnapshot>>(
            ErrorCode::too_large, "The grouped task publication could not be allocated.",
            "Retain the prior complete panel generation.");
    }
}

Result<std::vector<launcher::DesktopFileId>>
withPinnedApplication(const PinStateSnapshot &pins, const launcher::DesktopFileId &application) {
    if (pins.generation == 0U || pins.pins.size() > maximumPinCount) {
        return failure<std::vector<launcher::DesktopFileId>>(
            ErrorCode::validation_error, "The pin mutation source is invalid.",
            "Reload the complete private pin generation.");
    }
    if (std::ranges::any_of(pins.pins, [&application](const auto &pin) {
            return pin.value() == application.value();
        })) {
        return Result<std::vector<launcher::DesktopFileId>>::success(pins.pins);
    }
    if (pins.pins.size() >= maximumPinCount) {
        return failure<std::vector<launcher::DesktopFileId>>(
            ErrorCode::too_large, "The pin set is full.", "Remove a pin before adding another.");
    }
    auto replacement = pins.pins;
    replacement.push_back(application);
    return Result<std::vector<launcher::DesktopFileId>>::success(std::move(replacement));
}

Result<std::vector<launcher::DesktopFileId>>
withoutPinnedApplication(const PinStateSnapshot &pins, const launcher::DesktopFileId &application) {
    if (pins.generation == 0U || pins.pins.size() > maximumPinCount) {
        return failure<std::vector<launcher::DesktopFileId>>(
            ErrorCode::validation_error, "The pin mutation source is invalid.",
            "Reload the complete private pin generation.");
    }
    auto replacement = pins.pins;
    std::erase_if(replacement,
                  [&application](const auto &pin) { return pin.value() == application.value(); });
    return Result<std::vector<launcher::DesktopFileId>>::success(std::move(replacement));
}

std::optional<std::size_t> groupForTaskLifetime(const GroupedTaskSnapshot &snapshot,
                                                x11::TaskLifetimeId lifetime) noexcept {
    for (std::size_t groupIndex = 0U; groupIndex < snapshot.groups.size(); ++groupIndex) {
        if (std::ranges::any_of(snapshot.groups[groupIndex].members,
                                [lifetime](const PanelTaskMember &member) {
                                    return member.lifetime == lifetime;
                                })) {
            return groupIndex;
        }
    }
    return std::nullopt;
}

} // namespace prismdrake::shell::panel
