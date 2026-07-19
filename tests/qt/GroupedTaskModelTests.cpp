#include "GroupedTaskModel.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::panel {
namespace {

struct TaskFixture final {
    std::uint32_t window;
    std::uint64_t incarnation;
    std::string title;
    std::optional<std::string> desktopFileId;
    bool minimized{false};
    bool urgent{false};
    bool modal{false};
};

[[nodiscard]] launcher::DesktopFileId desktopId(std::string value) {
    auto id = launcher::deriveDesktopFileId(value);
    if (!id) {
        throw std::runtime_error{id.error().message};
    }
    return std::move(id).value();
}

[[nodiscard]] x11::WindowId window(std::uint32_t value) {
    return x11::WindowId::fromProtocol(value).value();
}

[[nodiscard]] x11::WindowIncarnationId incarnation(std::uint64_t value) {
    return x11::WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] x11::DecodedTaskObservation decoded(const TaskFixture &fixture) {
    x11::WindowMetadata metadata{
        window(fixture.window),
        fixture.title,
        x11::ApplicationIdentityEvidence{x11::ApplicationIdentitySource::wmClass, std::nullopt,
                                         "Fixture", "fixture"},
        x11::WindowType::normal,
        true,
        {},
        2U,
        false,
        fixture.minimized,
        fixture.urgent,
        false,
        fixture.modal,
        std::nullopt,
        {}};
    return {metadata.window, incarnation(fixture.incarnation), std::move(metadata),
            "application-x-executable"};
}

[[nodiscard]] std::shared_ptr<const x11::TaskModelSnapshot>
taskSnapshot(x11::TaskModel &model, const std::vector<TaskFixture> &fixtures,
             std::optional<std::uint32_t> active = std::nullopt) {
    std::vector<x11::DecodedTaskObservation> observations;
    std::vector<std::uint32_t> order;
    for (const auto &fixture : fixtures) {
        observations.push_back(decoded(fixture));
        order.push_back(fixture.window);
    }
    auto authoritative = x11::buildEwmhTaskListSnapshot(
        x11::EwmhTaskListObservation{std::move(order), std::nullopt, active});
    if (!authoritative) {
        throw std::runtime_error{authoritative.error().message};
    }
    auto snapshot = model.publish(
        x11::TaskModelObservation{std::move(authoritative).value(), std::move(observations)});
    if (!snapshot) {
        throw std::runtime_error{snapshot.error().message};
    }
    return std::move(snapshot).value();
}

[[nodiscard]] std::shared_ptr<const launcher::ApplicationAssociationSnapshot>
associations(const std::vector<TaskFixture> &fixtures, std::uint64_t generation = 1U) {
    std::vector<launcher::CatalogApplicationIdentity> catalog;
    std::vector<launcher::WindowAssociationObservation> windows;
    for (const auto &fixture : fixtures) {
        std::optional<launcher::DesktopFileId> activation;
        if (fixture.desktopFileId) {
            activation = desktopId(*fixture.desktopFileId);
            if (std::ranges::none_of(catalog, [&activation](const auto &entry) {
                    return entry.desktopFileId.value() == activation->value();
                })) {
                catalog.push_back({*activation, std::nullopt});
            }
        }
        windows.push_back(
            {launcher::ObservedWindowIdentity::fromObservation(fixture.window, fixture.incarnation)
                 .value(),
             std::move(activation), std::nullopt, std::nullopt});
    }
    auto result = launcher::associateApplications(catalog, windows, generation);
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] std::shared_ptr<const launcher::ApplicationCatalogSnapshot>
catalog(std::vector<std::pair<std::string, std::string>> applications,
        std::uint64_t generation = 1U) {
    std::vector<launcher::DiscoveredDesktopEntry> entries;
    std::vector<std::size_t> indices;
    for (std::size_t index = 0U; index < applications.size(); ++index) {
        auto id = desktopId(applications[index].first);
        launcher::DesktopEntry entry;
        entry.name = applications[index].second;
        entry.icon = "icon-" + std::to_string(index);
        auto location = launcher::makeDiscoveredDesktopFileLocation("/tmp/applications",
                                                                    applications[index].first, 0U);
        if (!location) {
            throw std::runtime_error{location.error().message};
        }
        entries.push_back({std::move(id), std::move(entry),
                           launcher::DesktopEntryVisibilityReason::visibleByDefault,
                           std::move(location).value()});
        indices.push_back(index);
    }
    auto discovery = std::make_shared<const launcher::DesktopEntryDiscoverySnapshot>(
        launcher::DesktopEntryDiscoverySnapshot{
            std::move(entries), indices, {}, applications.size(), true, false, false});
    std::vector<launcher::ApplicationCatalogDecision> decisions;
    for (const auto index : indices) {
        decisions.push_back(
            {index, launcher::ApplicationCatalogEligibilityReason::eligibleWithoutTryExec});
    }
    return std::make_shared<const launcher::ApplicationCatalogSnapshot>(
        launcher::ApplicationCatalogSnapshot{generation, discovery, std::move(decisions), indices,
                                             indices.size(), indices.size(), true});
}

[[nodiscard]] std::shared_ptr<const PinStateSnapshot> pins(std::vector<std::string> values,
                                                           std::uint64_t generation = 1U) {
    std::vector<launcher::DesktopFileId> identifiers;
    for (auto &value : values) {
        identifiers.push_back(desktopId(std::move(value)));
    }
    return std::make_shared<const PinStateSnapshot>(
        PinStateSnapshot{generation, std::move(identifiers), PinStateLoadStatus::loaded});
}

[[nodiscard]] GroupedTaskInput
input(const std::shared_ptr<const x11::TaskModelSnapshot> &tasks,
      const std::shared_ptr<const launcher::ApplicationAssociationSnapshot> &associations,
      const std::shared_ptr<const launcher::ApplicationCatalogSnapshot> &catalog,
      const std::shared_ptr<const PinStateSnapshot> &pins, config::GroupingMode grouping,
      double width = 1000.0) {
    return {tasks, associations, catalog, pins, grouping, width, 44.0, 4.0, 1U, 1U};
}

TEST(GroupedTaskModelTest, AlwaysCombinesPinsRunningApplicationsAndUnknownLifetimes) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor A", "editor.desktop"},
                               TaskFixture{11U, 110U, "Editor B", "editor.desktop", true},
                               TaskFixture{20U, 200U, "Terminal", "terminal.desktop", false, true},
                               TaskFixture{30U, 300U, "Unknown", std::nullopt, false, false, true}};
    x11::TaskModel owner;
    const auto taskState = taskSnapshot(owner, fixtures, 10U);
    const auto result = buildGroupedTasks(input(taskState, associations(fixtures),
                                                catalog({{"editor.desktop", "Editor"},
                                                         {"terminal.desktop", "Terminal"},
                                                         {"missing.desktop", "Missing"}}),
                                                pins({"editor.desktop", "missing.desktop"}),
                                                config::GroupingMode::always));

    ASSERT_TRUE(result);
    const auto &groups = result.value()->groups;
    ASSERT_EQ(groups.size(), 4U);
    EXPECT_EQ(groups[0].displayName, "Editor");
    EXPECT_TRUE(groups[0].pinned);
    EXPECT_EQ(groups[0].members.size(), 2U);
    EXPECT_TRUE(groups[0].active);
    EXPECT_FALSE(groups[0].minimized);
    EXPECT_EQ(groups[0].primaryAction, PanelTaskPrimaryAction::openGroup);
    EXPECT_EQ(groups[1].displayName, "Missing");
    EXPECT_TRUE(groups[1].pinned);
    EXPECT_TRUE(groups[1].members.empty());
    EXPECT_EQ(groups[1].primaryAction, PanelTaskPrimaryAction::launch);
    EXPECT_EQ(groups[2].displayName, "Terminal");
    EXPECT_TRUE(groups[2].urgent);
    EXPECT_EQ(groups[3].kind, PanelTaskGroupKind::unknown);
    EXPECT_EQ(groups[3].members.size(), 1U);
    EXPECT_TRUE(groups[3].modal);
}

TEST(GroupedTaskModelTest, NeverKeepsOneButtonPerWindowAndMissingPins) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor A", "editor.desktop"},
                               TaskFixture{11U, 110U, "Editor B", "editor.desktop"},
                               TaskFixture{20U, 200U, "Terminal", "terminal.desktop"}};
    x11::TaskModel owner;
    const auto taskState = taskSnapshot(owner, fixtures);
    const auto result = buildGroupedTasks(input(taskState, associations(fixtures),
                                                catalog({{"editor.desktop", "Editor"},
                                                         {"terminal.desktop", "Terminal"},
                                                         {"missing.desktop", "Missing"}}),
                                                pins({"editor.desktop", "missing.desktop"}),
                                                config::GroupingMode::never));

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value()->groups.size(), 4U);
    EXPECT_TRUE(result.value()->groups[0].pinned);
    EXPECT_EQ(result.value()->groups[0].members.front().title, "Editor A");
    EXPECT_TRUE(result.value()->groups[1].pinned);
    EXPECT_EQ(result.value()->groups[1].members.front().title, "Editor B");
    EXPECT_TRUE(result.value()->groups[2].members.empty());
    EXPECT_FALSE(result.value()->groups[3].pinned);
    EXPECT_EQ(result.value()->groups[3].displayName, "Terminal");
}

TEST(GroupedTaskModelTest, WhenFullUsesMinimumTargetsAndPreservesFocusedLifetime) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor A", "editor.desktop"},
                               TaskFixture{11U, 110U, "Editor B", "editor.desktop"}};
    x11::TaskModel owner;
    const auto taskState = taskSnapshot(owner, fixtures);
    const auto associationState = associations(fixtures);
    const auto catalogState = catalog({{"editor.desktop", "Editor"}});
    const auto pinState = pins({});

    const auto wide = buildGroupedTasks(input(taskState, associationState, catalogState, pinState,
                                              config::GroupingMode::when_full, 92.0));
    ASSERT_TRUE(wide);
    EXPECT_EQ(wide.value()->effectiveGrouping, config::GroupingMode::never);
    EXPECT_EQ(wide.value()->groups.size(), 2U);
    const auto focused = wide.value()->groups[1].members.front().lifetime;

    const auto narrow = buildGroupedTasks(input(taskState, associationState, catalogState, pinState,
                                                config::GroupingMode::when_full, 91.0));
    ASSERT_TRUE(narrow);
    EXPECT_EQ(narrow.value()->effectiveGrouping, config::GroupingMode::always);
    ASSERT_EQ(narrow.value()->groups.size(), 1U);
    EXPECT_EQ(groupForTaskLifetime(*narrow.value(), focused), 0U);
}

TEST(GroupedTaskModelTest, PinMutationsAreOrderedIdempotentAndBounded) {
    const auto state = pins({"first.desktop"});
    const auto second = desktopId("second.desktop");
    const auto added = withPinnedApplication(*state, second);
    ASSERT_TRUE(added);
    ASSERT_EQ(added.value().size(), 2U);
    EXPECT_EQ(added.value()[1].value(), "second.desktop");

    const auto unchanged = withPinnedApplication(*state, state->pins.front());
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged.value(), state->pins);

    const auto removed = withoutPinnedApplication(
        PinStateSnapshot{2U, added.value(), PinStateLoadStatus::loaded}, second);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed.value(), state->pins);
}

TEST(GroupedTaskModelTest, RejectsMixedOrMalformedCompleteInputs) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor", "editor.desktop"}};
    x11::TaskModel owner;
    auto value =
        input(taskSnapshot(owner, fixtures), associations(fixtures),
              catalog({{"editor.desktop", "Editor"}}), pins({}), config::GroupingMode::always);
    value.settingsGeneration = 0U;
    EXPECT_FALSE(buildGroupedTasks(value));
    value.settingsGeneration = 1U;
    value.availableTaskStripWidth = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(buildGroupedTasks(value));
    value.availableTaskStripWidth = 100.0;
    value.grouping = static_cast<config::GroupingMode>(255);
    EXPECT_FALSE(buildGroupedTasks(value));
}

} // namespace
} // namespace prismdrake::shell::panel
