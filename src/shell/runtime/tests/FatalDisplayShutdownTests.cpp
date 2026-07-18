#include "FatalDisplayShutdown.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] foundation::Error loss(std::string message) {
    return {foundation::ErrorCode::io_error, std::move(message),
            "Allow the session supervisor to restart prismdrake-shell."};
}

TEST(FatalDisplayShutdownTest, RejectsMissingProcessShutdownCallback) {
    const auto gate = FatalDisplayShutdown::create({});

    ASSERT_FALSE(gate);
    EXPECT_EQ(gate.error().code, foundation::ErrorCode::invalid_argument);
}

TEST(FatalDisplayShutdownTest, RequestsExactlyOneShutdownAcrossTransportReporters) {
    std::size_t shutdownCount = 0;
    std::string reportedMessage;
    auto gate = FatalDisplayShutdown::create([&](const foundation::Error &error) {
        ++shutdownCount;
        reportedMessage = error.message;
    });
    ASSERT_TRUE(gate) << gate.error().message;

    gate.value()->request(loss("The task transport was lost."));
    gate.value()->request(loss("The panel transport was lost."));

    EXPECT_TRUE(gate.value()->requested());
    EXPECT_EQ(shutdownCount, 1U);
    EXPECT_EQ(reportedMessage, "The task transport was lost.");
}

TEST(FatalDisplayShutdownTest, MarksShutdownBeforeInvokingReentrantCallback) {
    std::size_t shutdownCount = 0;
    FatalDisplayShutdown *gatePointer = nullptr;
    auto gate = FatalDisplayShutdown::create([&](const foundation::Error &error) {
        ++shutdownCount;
        gatePointer->request(error);
    });
    ASSERT_TRUE(gate) << gate.error().message;
    gatePointer = gate.value().get();

    gate.value()->request(loss("The X11 connection was lost."));

    EXPECT_TRUE(gate.value()->requested());
    EXPECT_EQ(shutdownCount, 1U);
}

} // namespace
} // namespace prismdrake::shell::runtime
