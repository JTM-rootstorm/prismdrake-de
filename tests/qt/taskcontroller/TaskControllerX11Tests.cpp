#include "TaskController.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>

#include <cstdlib>
#include <string>
#include <vector>

namespace prismdrake::shell::taskcontroller {
namespace {

TEST(TaskControllerX11IntegrationTest, OwnsOneEventPipelineAndFailsSoftWithoutAWindowManager) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    int argc = 1;
    char executable[] = "task-controller-x11-test";
    char *argv[] = {executable, nullptr};
    QCoreApplication application(argc, argv);
    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    bool connectionLost = false;

    auto controller = TaskController::create(
        presentation, display,
        [&connectionLost](const foundation::Error &) { connectionLost = true; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);
    EXPECT_FALSE(controller.value()->terminated());
    EXPECT_FALSE(connectionLost);
    EXPECT_EQ(presentation.rowCount(), 0);
    ASSERT_FALSE(recoverable.empty());
    EXPECT_EQ(recoverable.front().code, foundation::ErrorCode::not_found);
}

} // namespace
} // namespace prismdrake::shell::taskcontroller
