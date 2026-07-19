#include "GroupedTaskModel.hpp"
#include "GroupedTaskPresentationModel.hpp"
#include "PanelClock.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QSignalSpy>
#include <QTime>

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
      double width = 1000.0, std::uint64_t widthGeneration = 1U,
      std::uint64_t settingsGeneration = 1U) {
    return {tasks, associations, catalog, pins, grouping, width, 44.0, 4.0, settingsGeneration,
            widthGeneration};
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

TEST(GroupedTaskPresentationModelTest, EmitsOnlyTypedIntentsAndKeepsIdentitiesPrivate) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor A", "editor.desktop"},
                               TaskFixture{11U, 110U, "Editor B", "editor.desktop"},
                               TaskFixture{20U, 200U, "Terminal", "terminal.desktop"}};
    x11::TaskModel owner;
    const auto built = buildGroupedTasks(
        input(taskSnapshot(owner, fixtures, 10U), associations(fixtures),
              catalog({{"editor.desktop", "Editor"},
                       {"terminal.desktop", "Terminal"},
                       {"missing.desktop", "Missing"}}),
              pins({"editor.desktop", "missing.desktop"}), config::GroupingMode::always));
    ASSERT_TRUE(built);

    GroupedTaskPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(built.value()));
    ASSERT_EQ(model.rowCount(), 3);
    auto *editor = model.groupAt(0);
    auto *missing = model.groupAt(1);
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(editor->displayName(), QStringLiteral("Editor"));
    EXPECT_EQ(editor->windowCount(), 2);
    EXPECT_TRUE(editor->primaryOpensGroup());
    EXPECT_TRUE(editor->pinned());
    EXPECT_TRUE(missing->launchAvailable());
    EXPECT_FALSE(missing->running());

    const auto *groupMeta = editor->metaObject();
    EXPECT_EQ(groupMeta->indexOfProperty("desktopFileId"), -1);
    EXPECT_EQ(groupMeta->indexOfProperty("generation"), -1);
    const auto members = editor->members();
    ASSERT_EQ(members.size(), 2);
    auto *firstMember =
        qobject_cast<GroupedTaskMemberPresentation *>(members.front().value<QObject *>());
    ASSERT_NE(firstMember, nullptr);
    EXPECT_EQ(firstMember->metaObject()->indexOfProperty("lifetime"), -1);

    QObject *popupTarget = nullptr;
    QObject::connect(&model, &GroupedTaskPresentationModel::groupPopupRequested, &model,
                     [&popupTarget](QObject *group) { popupTarget = group; });
    EXPECT_TRUE(editor->requestPrimaryAction());
    EXPECT_EQ(popupTarget, editor);

    std::uint64_t activatedLifetime = 0U;
    std::uint64_t activatedGeneration = 0U;
    QObject::connect(&model, &GroupedTaskPresentationModel::activationRequested, &model,
                     [&activatedLifetime, &activatedGeneration](
                         x11::TaskLifetimeId lifetime, x11::TaskModelGeneration generation) {
                         activatedLifetime = lifetime.value();
                         activatedGeneration = generation.value();
                     });
    EXPECT_TRUE(firstMember->requestActivation());
    EXPECT_NE(activatedLifetime, 0U);
    EXPECT_EQ(activatedGeneration, built.value()->taskGeneration.value());

    std::string launched;
    QObject::connect(&model, &GroupedTaskPresentationModel::launchRequested, &model,
                     [&launched](launcher::DesktopFileId id) { launched = id.value(); });
    EXPECT_TRUE(missing->requestPrimaryAction());
    EXPECT_EQ(launched, "missing.desktop");

    std::string pinChanged;
    bool pinValue = true;
    QObject::connect(&model, &GroupedTaskPresentationModel::pinChangeRequested, &model,
                     [&pinChanged, &pinValue](launcher::DesktopFileId id, bool pinnedValue) {
                         pinChanged = id.value();
                         pinValue = pinnedValue;
                     });
    EXPECT_TRUE(missing->requestPinToggle());
    EXPECT_EQ(pinChanged, "missing.desktop");
    EXPECT_FALSE(pinValue);
}

TEST(GroupedTaskPresentationModelTest, RecoversExactMemberAcrossWhenFullRegrouping) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor A", "editor.desktop"},
                               TaskFixture{11U, 110U, "Editor B", "editor.desktop"}};
    x11::TaskModel owner;
    const auto taskState = taskSnapshot(owner, fixtures);
    const auto associationState = associations(fixtures);
    const auto catalogState = catalog({{"editor.desktop", "Editor"}});
    const auto pinState = pins({});
    const auto wide = buildGroupedTasks(input(taskState, associationState, catalogState, pinState,
                                              config::GroupingMode::when_full, 92.0, 1U));
    const auto narrow = buildGroupedTasks(input(taskState, associationState, catalogState, pinState,
                                                config::GroupingMode::when_full, 91.0, 2U));
    ASSERT_TRUE(wide);
    ASSERT_TRUE(narrow);

    GroupedTaskPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(wide.value()));
    ASSERT_EQ(model.rowCount(), 2);
    QPointer<GroupedTaskPresentation> oldFocused = model.groupAt(1);
    ASSERT_TRUE(model.rememberFocus(oldFocused, true));
    QSignalSpy recovery{&model, &GroupedTaskPresentationModel::focusRecoveryRequested};

    ASSERT_TRUE(model.applySnapshot(narrow.value()));
    EXPECT_TRUE(oldFocused.isNull());
    ASSERT_EQ(recovery.count(), 1);
    const auto arguments = recovery.takeFirst();
    EXPECT_EQ(arguments[0].toInt(), 0);
    EXPECT_EQ(arguments[1].toInt(), 1);
    EXPECT_TRUE(arguments[2].toBool());
}

TEST(GroupedTaskPresentationModelTest, AcceptsReplacementSettingsOwnerGeneration) {
    const std::vector fixtures{TaskFixture{10U, 100U, "Editor", "editor.desktop"}};
    x11::TaskModel owner;
    const auto taskState = taskSnapshot(owner, fixtures);
    const auto associationState = associations(fixtures);
    const auto catalogState = catalog({{"editor.desktop", "Editor"}});
    const auto pinState = pins({});
    const auto previous = buildGroupedTasks(
        input(taskState, associationState, catalogState, pinState, config::GroupingMode::always,
              1000.0, 1U, 3U));
    const auto replacement = buildGroupedTasks(
        input(taskState, associationState, catalogState, pinState, config::GroupingMode::always,
              1000.0, 2U, 1U));
    ASSERT_TRUE(previous);
    ASSERT_TRUE(replacement);

    GroupedTaskPresentationModel model;
    ASSERT_TRUE(model.applySnapshot(previous.value()));
    EXPECT_FALSE(model.applySnapshot(replacement.value()));
    auto *retained = model.groupAt(0);
    ASSERT_NE(retained, nullptr);

    model.clearSettingsOwner();
    EXPECT_FALSE(model.currentSnapshot());
    EXPECT_FALSE(retained->requestPrimaryAction());
    ASSERT_TRUE(model.applySnapshot(replacement.value()));
    EXPECT_EQ(model.groupAt(0), retained);
    EXPECT_EQ(model.currentSnapshot(), replacement.value());
}

TEST(PanelClockTest, FormatsLocaleTwelveAndTwentyFourHourModesDeterministically) {
    const QDateTime time{QDate{2026, 7, 18}, QTime{13, 5, 9, 250}};
    const QLocale locale{QLocale::English, QLocale::UnitedStates};
    const auto local = formatPanelClock(time, locale, config::ClockFormat::locale);
    const auto twelve = formatPanelClock(time, locale, config::ClockFormat::hour_12);
    const auto twentyFour = formatPanelClock(time, locale, config::ClockFormat::hour_24);
    ASSERT_TRUE(local);
    ASSERT_TRUE(twelve);
    ASSERT_TRUE(twentyFour);
    EXPECT_FALSE(local.value().isEmpty());
    EXPECT_EQ(twelve.value(), QStringLiteral("1:05 PM"));
    EXPECT_EQ(twentyFour.value(), QStringLiteral("13:05"));
    EXPECT_EQ(nextPanelClockUpdate(time).value(), std::chrono::milliseconds{50'750});
    EXPECT_FALSE(formatPanelClock(time, locale, static_cast<config::ClockFormat>(255)));
    EXPECT_FALSE(nextPanelClockUpdate(QDateTime{}));
}

} // namespace
} // namespace prismdrake::shell::panel
