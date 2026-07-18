#include "TerminationSignalBridge.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <gtest/gtest.h>

#include <csignal>

namespace prismdrake::shell::runtime {
namespace {

void expectSignalDelivery(int signalNumber) {
    QEventLoop eventLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(1000);
    QObject::connect(&timeout, &QTimer::timeout, &eventLoop, &QEventLoop::quit);

    bool callbackInvoked = false;
    auto bridge = TerminationSignalBridge::create([&]() {
        callbackInvoked = true;
        eventLoop.quit();
    });
    ASSERT_TRUE(bridge) << bridge.error().message;

    timeout.start();
    ASSERT_EQ(::raise(signalNumber), 0);
    eventLoop.exec();

    EXPECT_TRUE(callbackInvoked);
}

TEST(TerminationSignalBridgeTest, DeliversSigtermOnTheQtOwnerThread) {
    expectSignalDelivery(SIGTERM);
}

TEST(TerminationSignalBridgeTest, DeliversSigintOnTheQtOwnerThread) {
    expectSignalDelivery(SIGINT);
}

} // namespace
} // namespace prismdrake::shell::runtime

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
