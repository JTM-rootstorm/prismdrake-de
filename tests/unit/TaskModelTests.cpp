#include "TaskModel.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::x11 {
namespace {

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

[[nodiscard]] WindowIncarnationId incarnation(std::uint64_t value) {
    return WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] WindowMetadata metadata(std::uint32_t windowValue, std::string title,
                                      WindowType type = WindowType::normal) {
    return WindowMetadata{window(windowValue),
                          std::move(title),
                          ApplicationIdentityEvidence{ApplicationIdentitySource::wmClass,
                                                      std::nullopt, "Fixture", "fixture.desktop"},
                          type,
                          true,
                          {},
                          2U,
                          false,
                          false,
                          false,
                          false,
                          false,
                          std::nullopt,
                          {}};
}

[[nodiscard]] DecodedTaskObservation decoded(std::uint64_t incarnationValue, WindowMetadata value,
                                             std::string icon = "application-x-executable") {
    const auto id = value.window;
    return DecodedTaskObservation{id, incarnation(incarnationValue), std::move(value),
                                  std::move(icon)};
}

[[nodiscard]] DecodedTaskObservation unavailable(std::uint32_t windowValue,
                                                 std::uint64_t incarnationValue) {
    return DecodedTaskObservation{
        window(windowValue), incarnation(incarnationValue), std::nullopt, {}};
}

[[nodiscard]] EwmhTaskListSnapshot
authoritative(std::vector<std::uint32_t> clients,
              std::optional<std::uint32_t> active = std::nullopt) {
    auto snapshot = buildEwmhTaskListSnapshot(
        EwmhTaskListObservation{std::move(clients), std::nullopt, active});
    EXPECT_TRUE(snapshot);
    return std::move(snapshot).value();
}

TEST(TaskModelTest, PublishesOrderedAuthoritativeFieldsAndExcludesNonTasks) {
    TaskModel model;
    auto parent = metadata(101U, "Parent");
    parent.minimized = true;
    parent.urgent = true;
    auto child = metadata(102U, "Dialog", WindowType::dialog);
    child.transientFor = window(101U);
    child.modal = true;
    auto skip = metadata(105U, "Skip");
    skip.skipTaskbar = true;

    const auto published = model.publish(
        TaskModelObservation{authoritative({101U, 102U, 103U, 104U, 105U}, 102U),
                             {decoded(11U, std::move(parent), "parent-icon"),
                              decoded(12U, std::move(child), "dialog-icon"),
                              decoded(13U, metadata(103U, "Dock", WindowType::dock)),
                              decoded(14U, metadata(104U, "Desktop", WindowType::desktop)),
                              decoded(15U, std::move(skip))}});

    ASSERT_TRUE(published);
    ASSERT_EQ(published.value()->tasks().size(), 2U);
    const auto &first = published.value()->tasks()[0];
    const auto &second = published.value()->tasks()[1];
    EXPECT_EQ(first.window(), window(101U));
    EXPECT_EQ(first.incarnation(), incarnation(11U));
    EXPECT_EQ(first.title(), "Parent");
    EXPECT_EQ(first.applicationId(), "fixture.desktop");
    EXPECT_EQ(first.applicationIdentitySource(), ApplicationIdentitySource::wmClass);
    EXPECT_EQ(first.fallbackIconName(), "parent-icon");
    EXPECT_FALSE(first.active());
    EXPECT_TRUE(first.hidden());
    EXPECT_TRUE(first.urgent());
    EXPECT_EQ(first.workspace(), 2U);
    EXPECT_FALSE(first.onAllWorkspaces());
    EXPECT_EQ(first.type(), WindowType::normal);
    EXPECT_TRUE(second.active());
    ASSERT_TRUE(second.transientParent());
    EXPECT_EQ(second.transientParent().value(), first.lifetime());
    EXPECT_TRUE(second.modal());
    EXPECT_EQ(first.lastObservedGeneration(), published.value()->generation());
    ASSERT_EQ(published.value()->authoritativeClients().size(), 5U);
    EXPECT_TRUE(published.value()->authoritativelyContains(window(101U), incarnation(11U)));
    EXPECT_TRUE(published.value()->authoritativelyContains(window(105U), incarnation(15U)));
    EXPECT_FALSE(published.value()->authoritativelyContains(window(101U), incarnation(99U)));
}

TEST(TaskModelTest, ExcludesAuxiliaryTypesButKeepsPd1TaskTypes) {
    TaskModel model;
    const std::vector<WindowType> types{
        WindowType::dialog,    WindowType::utility,    WindowType::toolbar,
        WindowType::menu,      WindowType::splash,     WindowType::dropdownMenu,
        WindowType::popupMenu, WindowType::tooltip,    WindowType::notification,
        WindowType::combo,     WindowType::dragAndDrop};
    std::vector<std::uint32_t> clients;
    std::vector<DecodedTaskObservation> windows;
    for (std::size_t index = 0U; index < types.size(); ++index) {
        const auto id = static_cast<std::uint32_t>(index + 1U);
        clients.push_back(id);
        windows.push_back(decoded(index + 1U, metadata(id, "fixture", types[index])));
    }

    const auto published =
        model.publish(TaskModelObservation{authoritative(std::move(clients)), std::move(windows)});
    ASSERT_TRUE(published);
    ASSERT_EQ(published.value()->tasks().size(), 5U);
    EXPECT_EQ(published.value()->tasks()[0].type(), WindowType::dialog);
    EXPECT_EQ(published.value()->tasks()[4].type(), WindowType::splash);
}

TEST(TaskModelTest, ReorderAdvancesGenerationWithoutChangingLifetimeIdentity) {
    TaskModel model;
    auto initial = model.publish(
        TaskModelObservation{authoritative({10U, 20U}, 10U),
                             {decoded(1U, metadata(10U, "A")), decoded(2U, metadata(20U, "B"))}});
    ASSERT_TRUE(initial);
    const auto firstLifetime = initial.value()->tasks()[0].lifetime();
    const auto secondLifetime = initial.value()->tasks()[1].lifetime();

    auto reordered = model.publish(
        TaskModelObservation{authoritative({20U, 10U}, 20U),
                             {decoded(1U, metadata(10U, "A2")), decoded(2U, metadata(20U, "B2"))}});
    ASSERT_TRUE(reordered);
    EXPECT_EQ(reordered.value()->generation().value(), 2U);
    EXPECT_EQ(reordered.value()->tasks()[0].window(), window(20U));
    EXPECT_EQ(reordered.value()->tasks()[0].lifetime(), secondLifetime);
    EXPECT_EQ(reordered.value()->tasks()[1].lifetime(), firstLifetime);
    EXPECT_TRUE(reordered.value()->tasks()[0].active());
}

TEST(TaskModelTest, RemovesStaleWindowsAndKeepsPriorSnapshotsImmutable) {
    TaskModel model;
    auto first = model.publish(
        TaskModelObservation{authoritative({10U, 20U}),
                             {decoded(1U, metadata(10U, "A")), decoded(2U, metadata(20U, "B"))}});
    ASSERT_TRUE(first);

    auto second = model.publish(
        TaskModelObservation{authoritative({20U}), {decoded(2U, metadata(20U, "B changed"))}});
    ASSERT_TRUE(second);
    ASSERT_EQ(second.value()->tasks().size(), 1U);
    EXPECT_EQ(second.value()->tasks()[0].window(), window(20U));
    ASSERT_EQ(first.value()->tasks().size(), 2U);
    EXPECT_EQ(first.value()->tasks()[0].title(), "A");
    EXPECT_EQ(model.current(), second.value());
}

TEST(TaskModelTest, XidReuseAlwaysAllocatesANewModelLifetime) {
    TaskModel model;
    auto first = model.publish(
        TaskModelObservation{authoritative({77U}), {decoded(100U, metadata(77U, "Old"))}});
    ASSERT_TRUE(first);
    const auto oldLifetime = first.value()->tasks()[0].lifetime();

    auto invalidFetch =
        model.publish(TaskModelObservation{authoritative({77U}, 77U), {unavailable(77U, 100U)}});
    ASSERT_TRUE(invalidFetch);
    EXPECT_TRUE(invalidFetch.value()->tasks().empty());

    auto recovered = model.publish(
        TaskModelObservation{authoritative({77U}), {decoded(100U, metadata(77U, "Recovered"))}});
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value()->tasks()[0].lifetime(), oldLifetime);

    auto reused = model.publish(
        TaskModelObservation{authoritative({77U}), {decoded(101U, metadata(77U, "New"))}});
    ASSERT_TRUE(reused);
    EXPECT_NE(reused.value()->tasks()[0].lifetime(), oldLifetime);

    auto disappeared = model.publish(TaskModelObservation{authoritative({}), {}});
    ASSERT_TRUE(disappeared);
    EXPECT_TRUE(disappeared.value()->tasks().empty());
    auto reusedAgain = model.publish(
        TaskModelObservation{authoritative({77U}), {decoded(102U, metadata(77U, "Newer"))}});
    ASSERT_TRUE(reusedAgain);
    EXPECT_NE(reusedAgain.value()->tasks()[0].lifetime(), reused.value()->tasks()[0].lifetime());
}

TEST(TaskModelTest, TemporarySkipTaskbarFilteringPreservesWindowLifetime) {
    TaskModel model;
    auto visible = model.publish(
        TaskModelObservation{authoritative({7U}), {decoded(70U, metadata(7U, "Visible"))}});
    ASSERT_TRUE(visible);
    const auto lifetime = visible.value()->tasks()[0].lifetime();

    auto skippedMetadata = metadata(7U, "Skipped");
    skippedMetadata.skipTaskbar = true;
    auto skipped = model.publish(
        TaskModelObservation{authoritative({7U}), {decoded(70U, std::move(skippedMetadata))}});
    ASSERT_TRUE(skipped);
    EXPECT_TRUE(skipped.value()->tasks().empty());

    auto restored = model.publish(
        TaskModelObservation{authoritative({7U}), {decoded(70U, metadata(7U, "Restored"))}});
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored.value()->tasks()[0].lifetime(), lifetime);
}

TEST(TaskModelTest, InvalidObservationNeverReplacesOrMutatesCurrentSnapshot) {
    TaskModel model;
    auto current = model.publish(
        TaskModelObservation{authoritative({10U}), {decoded(1U, metadata(10U, "private-title"))}});
    ASSERT_TRUE(current);

    const auto duplicate = model.publish(TaskModelObservation{
        authoritative({10U}),
        {decoded(1U, metadata(10U, "first")), decoded(2U, metadata(10U, "second"))}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(model.current(), current.value());

    auto transient = metadata(10U, "still present");
    transient.transientFor = window(20U);
    const auto incomplete = model.publish(
        TaskModelObservation{authoritative({10U, 20U}), {decoded(1U, std::move(transient))}});
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(model.current(), current.value());

    auto oversizedMetadata = metadata(30U, std::string(maximumWindowTitleBytes + 1U, 'x'));
    oversizedMetadata.identity.groupingKey = "secret-application-value";
    const auto oversized = model.publish(
        TaskModelObservation{authoritative({30U}), {decoded(3U, std::move(oversizedMetadata))}});
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, foundation::ErrorCode::too_large);
    EXPECT_EQ(oversized.error().message.find("secret-application-value"), std::string::npos);
    EXPECT_EQ(model.current(), current.value());

    const auto missingIcon = model.publish(
        TaskModelObservation{authoritative({40U}), {decoded(4U, metadata(40U, "No icon"), "")}});
    ASSERT_FALSE(missingIcon);
    EXPECT_EQ(model.current(), current.value());
}

TEST(TaskModelTest, RejectsAmbiguousIncarnationsAndTransientCycles) {
    TaskModel model;
    const auto duplicateIncarnation = model.publish(TaskModelObservation{
        authoritative({1U, 2U}), {decoded(9U, metadata(1U, "A")), decoded(9U, metadata(2U, "B"))}});
    EXPECT_FALSE(duplicateIncarnation);

    auto selfTransient = metadata(4U, "Self transient");
    selfTransient.transientFor = window(4U);
    const auto transientRejected = model.publish(
        TaskModelObservation{authoritative({4U}), {decoded(11U, std::move(selfTransient))}});
    EXPECT_FALSE(transientRejected);
    EXPECT_FALSE(model.current());
}

TEST(TaskModelTest, AlignsWithRootBoundAndRejectsOversizedMetadataSet) {
    EXPECT_EQ(maximumTaskWindows, 256U);
    TaskModel model;
    std::vector<DecodedTaskObservation> windows;
    windows.reserve(maximumTaskWindows + 1U);
    for (std::size_t index = 0U; index <= maximumTaskWindows; ++index) {
        windows.push_back(decoded(index + 1U, metadata(1U, "duplicate")));
    }
    const auto rejected =
        model.publish(TaskModelObservation{authoritative({1U}), std::move(windows)});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::too_large);
    EXPECT_FALSE(model.current());
}

} // namespace
} // namespace prismdrake::x11
