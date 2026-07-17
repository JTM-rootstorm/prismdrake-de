#include "TaskPresentationModel.hpp"

#include "EwmhTaskList.hpp"
#include "WindowMetadata.hpp"

#include <gtest/gtest.h>

#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace prismdrake::shell::tasks {
namespace {

using foundation::ErrorCode;
using prismdrake::x11::ApplicationIdentityEvidence;
using prismdrake::x11::ApplicationIdentitySource;
using prismdrake::x11::DecodedTaskObservation;
using prismdrake::x11::EwmhTaskListObservation;
using prismdrake::x11::TaskLifetimeId;
using prismdrake::x11::TaskModel;
using prismdrake::x11::TaskModelGeneration;
using prismdrake::x11::TaskModelObservation;
using prismdrake::x11::TaskModelSnapshot;
using prismdrake::x11::WindowId;
using prismdrake::x11::WindowIncarnationId;
using prismdrake::x11::WindowMetadata;
using prismdrake::x11::WindowType;

struct FixtureTask final {
    std::uint32_t window;
    std::uint64_t incarnation;
    std::string title;
    std::string applicationId;
    std::string icon;
    bool minimized{false};
    bool urgent{false};
    bool modal{false};
};

[[nodiscard]] WindowId window(std::uint32_t value) { return WindowId::fromProtocol(value).value(); }

[[nodiscard]] WindowIncarnationId incarnation(std::uint64_t value) {
    return WindowIncarnationId::fromObserved(value).value();
}

[[nodiscard]] DecodedTaskObservation decoded(const FixtureTask &task) {
    WindowMetadata metadata{window(task.window),
                            task.title,
                            ApplicationIdentityEvidence{ApplicationIdentitySource::wmClass,
                                                        std::nullopt, "Fixture",
                                                        task.applicationId},
                            WindowType::normal,
                            true,
                            {},
                            2U,
                            false,
                            task.minimized,
                            task.urgent,
                            false,
                            task.modal,
                            std::nullopt,
                            {}};
    return {metadata.window, incarnation(task.incarnation), std::move(metadata), task.icon};
}

[[nodiscard]] std::shared_ptr<const TaskModelSnapshot>
publish(TaskModel &model, const std::vector<FixtureTask> &tasks, std::vector<std::uint32_t> order,
        std::optional<std::uint32_t> active = std::nullopt) {
    std::vector<DecodedTaskObservation> observations;
    observations.reserve(tasks.size());
    for (const auto &task : tasks) {
        observations.push_back(decoded(task));
    }
    auto authoritative = prismdrake::x11::buildEwmhTaskListSnapshot(
        EwmhTaskListObservation{std::move(order), std::nullopt, active});
    if (!authoritative) {
        throw std::runtime_error{authoritative.error().message};
    }
    auto result = model.publish(
        TaskModelObservation{std::move(authoritative).value(), std::move(observations)});
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] FixtureTask task(std::uint32_t windowValue, std::uint64_t incarnationValue,
                               std::string title) {
    return {windowValue, incarnationValue, std::move(title),
            "fixture-" + std::to_string(windowValue) + ".desktop",
            "fixture-icon-" + std::to_string(windowValue)};
}

TEST(TaskPresentationModelTest, MirrorsBoundedStateWithoutNumericIdentityProperties) {
    TaskModel source;
    auto activeUrgent = task(10U, 100U, "Active task");
    activeUrgent.urgent = true;
    auto minimizedModal = task(20U, 200U, "Minimized dialog");
    minimizedModal.minimized = true;
    minimizedModal.modal = true;
    const auto snapshot = publish(source, {activeUrgent, minimizedModal}, {10U, 20U}, 10U);

    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(snapshot));
    ASSERT_EQ(presentation.rowCount(), 2);
    EXPECT_EQ(presentation.roleNames(),
              (QHash<int, QByteArray>{{TaskPresentationModel::taskObject, "task"}}));

    auto *first = presentation.taskAt(0);
    auto *second = presentation.taskAt(1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->title(), QStringLiteral("Active task"));
    EXPECT_EQ(first->applicationId(), QStringLiteral("fixture-10.desktop"));
    EXPECT_EQ(first->fallbackIconName(), QStringLiteral("fixture-icon-10"));
    EXPECT_TRUE(first->active());
    EXPECT_FALSE(first->minimized());
    EXPECT_TRUE(first->urgent());
    EXPECT_FALSE(first->modal());
    EXPECT_EQ(first->statusText(), QStringLiteral("Active, Urgent"));
    EXPECT_FALSE(second->active());
    EXPECT_TRUE(second->minimized());
    EXPECT_FALSE(second->urgent());
    EXPECT_TRUE(second->modal());
    EXPECT_EQ(second->statusText(), QStringLiteral("Minimized, Modal"));
    EXPECT_EQ(presentation.data(presentation.index(0), TaskPresentationModel::taskObject)
                  .value<QObject *>(),
              first);

    const auto *metadata = first->metaObject();
    EXPECT_EQ(metadata->indexOfProperty("windowId"), -1);
    EXPECT_EQ(metadata->indexOfProperty("incarnation"), -1);
    EXPECT_EQ(metadata->indexOfProperty("lifetime"), -1);
    EXPECT_EQ(metadata->indexOfProperty("generation"), -1);
    EXPECT_EQ(metadata->indexOfProperty("originatingGeneration"), -1);
}

TEST(TaskPresentationModelTest, EmitsExactTypedIntentsFromTheCurrentCoherentGeneration) {
    TaskModel source;
    const auto firstSnapshot = publish(source, {task(10U, 100U, "Task")}, {10U}, 10U);

    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(firstSnapshot));
    auto *object = presentation.taskAt(0);
    ASSERT_NE(object, nullptr);

    struct Capture final {
        std::string action;
        std::uint64_t lifetime{0U};
        std::uint64_t generation{0U};
    } capture;
    const auto record = [&capture](std::string action, TaskLifetimeId lifetime,
                                   TaskModelGeneration generation) {
        capture = {std::move(action), lifetime.value(), generation.value()};
    };
    QObject::connect(&presentation, &TaskPresentationModel::activationRequested, &presentation,
                     [&record](TaskLifetimeId lifetime, TaskModelGeneration generation) {
                         record("activate", lifetime, generation);
                     });
    QObject::connect(&presentation, &TaskPresentationModel::minimizationRequested, &presentation,
                     [&record](TaskLifetimeId lifetime, TaskModelGeneration generation) {
                         record("minimize", lifetime, generation);
                     });
    QObject::connect(&presentation, &TaskPresentationModel::closeRequested, &presentation,
                     [&record](TaskLifetimeId lifetime, TaskModelGeneration generation) {
                         record("close", lifetime, generation);
                     });

    ASSERT_TRUE(object->requestActivation());
    EXPECT_EQ(capture.action, "activate");
    EXPECT_EQ(capture.lifetime, firstSnapshot->tasks()[0].lifetime().value());
    EXPECT_EQ(capture.generation, firstSnapshot->generation().value());
    ASSERT_TRUE(object->requestMinimization());
    EXPECT_EQ(capture.action, "minimize");
    ASSERT_TRUE(object->requestClose());
    EXPECT_EQ(capture.action, "close");

    const auto nextSnapshot = publish(source, {task(10U, 100U, "Task")}, {10U}, 10U);
    ASSERT_TRUE(presentation.applySnapshot(nextSnapshot));
    ASSERT_EQ(presentation.taskAt(0), object);
    ASSERT_TRUE(object->requestActivation());
    EXPECT_EQ(capture.lifetime, nextSnapshot->tasks()[0].lifetime().value());
    EXPECT_EQ(capture.generation, nextSnapshot->generation().value());
}

TEST(TaskPresentationModelTest, PreservesUnchangedObjectsAcrossInsertReorderChangeAndRemove) {
    TaskModel source;
    const auto a = task(10U, 100U, "A");
    const auto b = task(20U, 200U, "B");
    const auto c = task(30U, 300U, "C");

    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(publish(source, {a, b}, {10U, 20U}, 10U)));
    auto *aObject = presentation.taskAt(0);
    auto *bObject = presentation.taskAt(1);

    ASSERT_TRUE(presentation.applySnapshot(publish(source, {a, b, c}, {10U, 20U, 30U}, 10U)));
    auto *cObject = presentation.taskAt(2);
    EXPECT_EQ(presentation.taskAt(0), aObject);
    EXPECT_EQ(presentation.taskAt(1), bObject);

    ASSERT_TRUE(presentation.applySnapshot(publish(source, {a, b, c}, {20U, 10U, 30U}, 10U)));
    EXPECT_EQ(presentation.taskAt(0), bObject);
    EXPECT_EQ(presentation.taskAt(1), aObject);
    EXPECT_EQ(presentation.taskAt(2), cObject);

    auto changedB = b;
    changedB.title = "B changed";
    changedB.urgent = true;
    ASSERT_TRUE(
        presentation.applySnapshot(publish(source, {a, changedB, c}, {20U, 10U, 30U}, 10U)));
    auto *changedBObject = presentation.taskAt(0);
    EXPECT_NE(changedBObject, bObject);
    EXPECT_EQ(changedBObject->statusText(), QStringLiteral("Urgent"));
    EXPECT_EQ(presentation.taskAt(1), aObject);
    EXPECT_EQ(presentation.taskAt(2), cObject);

    ASSERT_TRUE(
        presentation.applySnapshot(publish(source, {changedB, c}, {20U, 30U}, std::nullopt)));
    EXPECT_EQ(presentation.rowCount(), 2);
    EXPECT_EQ(presentation.taskAt(0), changedBObject);
    EXPECT_EQ(presentation.taskAt(1), cObject);
}

TEST(TaskPresentationModelTest, XidReuseCreatesANewPresentationAndTypedLifetime) {
    TaskModel source;
    TaskPresentationModel presentation;
    const auto first = publish(source, {task(77U, 100U, "Same")}, {77U}, 77U);
    ASSERT_TRUE(presentation.applySnapshot(first));
    QPointer<TaskPresentation> oldObject = presentation.taskAt(0);

    std::uint64_t emittedLifetime = 0U;
    QObject::connect(&presentation, &TaskPresentationModel::activationRequested, &presentation,
                     [&emittedLifetime](TaskLifetimeId lifetime, TaskModelGeneration) {
                         emittedLifetime = lifetime.value();
                     });
    ASSERT_TRUE(oldObject->requestActivation());
    const auto oldLifetime = emittedLifetime;

    const auto reused = publish(source, {task(77U, 101U, "Same")}, {77U}, 77U);
    ASSERT_TRUE(presentation.applySnapshot(reused));
    auto *newObject = presentation.taskAt(0);
    ASSERT_NE(newObject, nullptr);
    EXPECT_TRUE(oldObject.isNull());
    ASSERT_TRUE(newObject->requestActivation());
    EXPECT_NE(emittedLifetime, oldLifetime);
    EXPECT_EQ(emittedLifetime, reused->tasks()[0].lifetime().value());
}

TEST(TaskPresentationModelTest, RejectsNullStaleAndEqualGenerationConflictWithoutMutation) {
    TaskModel source;
    const auto first = publish(source, {task(10U, 100U, "First")}, {10U});
    const auto second = publish(source, {task(10U, 100U, "Second")}, {10U});

    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(second));
    auto *retained = presentation.taskAt(0);

    const auto absent = presentation.applySnapshot(nullptr);
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, ErrorCode::invalid_argument);
    const auto stale = presentation.applySnapshot(first);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::cancelled);

    std::shared_ptr<const TaskModelSnapshot> conflict =
        std::make_shared<TaskModelSnapshot>(*second);
    const auto conflicting = presentation.applySnapshot(std::move(conflict));
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().code, ErrorCode::validation_error);
    EXPECT_TRUE(presentation.applySnapshot(second));
    EXPECT_EQ(presentation.currentSnapshot(), second);
    EXPECT_EQ(presentation.taskAt(0), retained);
    EXPECT_EQ(presentation.taskAt(0)->title(), QStringLiteral("Second"));
}

TEST(TaskPresentationModelTest, RejectsCrossThreadApplicationAndRetainsPriorState) {
    TaskModel source;
    const auto first = publish(source, {task(10U, 100U, "First")}, {10U});
    const auto second = publish(source, {task(10U, 100U, "Second")}, {10U});
    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(first));
    auto *retained = presentation.taskAt(0);

    auto future = std::async(
        std::launch::async, [&presentation, second] { return presentation.applySnapshot(second); });
    const auto rejected = future.get();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::cancelled);
    EXPECT_EQ(presentation.currentSnapshot(), first);
    EXPECT_EQ(presentation.taskAt(0), retained);
}

TEST(TaskPresentationModelTest, RejectsReentrantApplyAndIntentUntilPublicationIsCoherent) {
    TaskModel source;
    const auto first = publish(source, {task(10U, 100U, "First")}, {10U});
    const auto second = publish(source, {task(10U, 100U, "Second")}, {10U});
    const auto third = publish(source, {task(10U, 100U, "Third")}, {10U});
    TaskPresentationModel presentation;
    ASSERT_TRUE(presentation.applySnapshot(first));

    std::optional<foundation::Result<void>> reentrant;
    bool requestAccepted = true;
    int intentCount = 0;
    QObject::connect(&presentation, &TaskPresentationModel::activationRequested, &presentation,
                     [&intentCount](TaskLifetimeId, TaskModelGeneration) { ++intentCount; });
    QObject::connect(&presentation, &TaskPresentationModel::publicationReconciliationStarted,
                     &presentation, [&] {
                         EXPECT_TRUE(presentation.isApplyingSnapshot());
                         requestAccepted = presentation.taskAt(0)->requestActivation();
                         reentrant.emplace(presentation.applySnapshot(third));
                     });
    bool appliedWasCoherent = false;
    QObject::connect(&presentation, &TaskPresentationModel::publicationApplied, &presentation, [&] {
        appliedWasCoherent = !presentation.isApplyingSnapshot() &&
                             presentation.currentSnapshot() == second &&
                             presentation.taskAt(0)->title() == QStringLiteral("Second");
    });

    ASSERT_TRUE(presentation.applySnapshot(second));
    ASSERT_TRUE(reentrant.has_value());
    EXPECT_FALSE(reentrant->hasValue());
    EXPECT_EQ(reentrant->error().code, ErrorCode::cancelled);
    EXPECT_FALSE(requestAccepted);
    EXPECT_EQ(intentCount, 0);
    EXPECT_TRUE(appliedWasCoherent);
    EXPECT_EQ(presentation.currentSnapshot(), second);
}

} // namespace
} // namespace prismdrake::shell::tasks
