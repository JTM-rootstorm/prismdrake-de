#pragma once

#include "ApplicationCatalog.hpp"
#include "ApplicationIdentity.hpp"
#include "Configuration.hpp"
#include "PinState.hpp"
#include "Result.hpp"
#include "TaskModel.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prismdrake::shell::panel {

enum class PanelTaskGroupKind : std::uint8_t { desktopEntry, wmClassFallback, unknown };
enum class PanelTaskPrimaryAction : std::uint8_t {
    unavailable,
    launch,
    activateOrMinimize,
    openGroup
};

struct PanelTaskMember final {
    x11::TaskLifetimeId lifetime;
    std::string title;
    bool active;
    bool minimized;
    bool urgent;
    bool modal;
};

/// One display-free panel target. The internal key and task lifetimes never cross into QML.
struct PanelTaskGroup final {
    PanelTaskGroupKind kind;
    std::string internalKey;
    std::optional<launcher::DesktopFileId> desktopFileId;
    std::string displayName;
    std::string iconName;
    bool pinned;
    bool launchAvailable;
    bool active;
    bool minimized;
    bool urgent;
    bool modal;
    PanelTaskPrimaryAction primaryAction;
    std::vector<PanelTaskMember> members;
};

struct GroupedTaskSnapshot final {
    x11::TaskModelGeneration taskGeneration;
    std::uint64_t associationGeneration;
    std::uint64_t catalogGeneration;
    std::uint64_t pinGeneration;
    std::uint64_t settingsGeneration;
    std::uint64_t widthGeneration;
    config::GroupingMode requestedGrouping;
    config::GroupingMode effectiveGrouping;
    std::vector<PanelTaskGroup> groups;
};

struct GroupedTaskInput final {
    std::shared_ptr<const x11::TaskModelSnapshot> tasks;
    std::shared_ptr<const launcher::ApplicationAssociationSnapshot> associations;
    std::shared_ptr<const launcher::ApplicationCatalogSnapshot> catalog;
    std::shared_ptr<const PinStateSnapshot> pins;
    config::GroupingMode grouping;
    double availableTaskStripWidth;
    double minimumTargetSize;
    double itemSpacing;
    std::uint64_t settingsGeneration;
    std::uint64_t widthGeneration;
};

/// Builds a complete immutable grouping publication or retains the caller's prior snapshot.
[[nodiscard]] foundation::Result<std::shared_ptr<const GroupedTaskSnapshot>>
buildGroupedTasks(const GroupedTaskInput &input);

/// Produces a complete pin replacement without mutating the store.
[[nodiscard]] foundation::Result<std::vector<launcher::DesktopFileId>>
withPinnedApplication(const PinStateSnapshot &pins, const launcher::DesktopFileId &application);
[[nodiscard]] foundation::Result<std::vector<launcher::DesktopFileId>>
withoutPinnedApplication(const PinStateSnapshot &pins, const launcher::DesktopFileId &application);

/// Finds the group that retains a previously focused task lifetime after regrouping.
[[nodiscard]] std::optional<std::size_t>
groupForTaskLifetime(const GroupedTaskSnapshot &snapshot, x11::TaskLifetimeId lifetime) noexcept;

} // namespace prismdrake::shell::panel
