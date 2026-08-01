#include "SingletonTaskPolicyGate.hpp"

#include <gtest/gtest.h>

namespace prismdrake::shell::runtime {
namespace {

using Disposition = SingletonTaskPolicyGate::CompletionDisposition;

TEST(SingletonTaskPolicyGateTest, NeverAllowsPresentationBeforeSuccessfulPublication) {
    SingletonTaskPolicyGate gate;

    EXPECT_FALSE(gate.ready());
    ASSERT_TRUE(gate.begin(1U));
    EXPECT_TRUE(gate.pending());
    EXPECT_FALSE(gate.ready());
    EXPECT_EQ(gate.complete(1U, true, true), Disposition::ready);
    EXPECT_FALSE(gate.pending());
    EXPECT_TRUE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, FailedCurrentPublicationRemainsFatalAndUnready) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(2U));
    EXPECT_EQ(gate.complete(2U, false, true), Disposition::fatal);
    EXPECT_FALSE(gate.pending());
    EXPECT_FALSE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, OwnerLossRetainsPendingGenerationUntilCompletionThenDiscards) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(3U));
    gate.invalidate();
    EXPECT_TRUE(gate.pending());
    EXPECT_FALSE(gate.ready());
    EXPECT_EQ(gate.complete(3U, false, false), Disposition::discard);
    EXPECT_FALSE(gate.pending());
    EXPECT_FALSE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, InvalidatedSuccessfulPublicationIsAlsoDiscarded) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(31U));
    gate.invalidate();
    EXPECT_EQ(gate.complete(31U, true, false), Disposition::discard);
    EXPECT_FALSE(gate.pending());
    EXPECT_FALSE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, OwnerLossClearsAnAlreadyReadyGeneration) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(32U));
    ASSERT_EQ(gate.complete(32U, true, true), Disposition::ready);
    gate.invalidate();
    EXPECT_FALSE(gate.pending());
    EXPECT_FALSE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, ReplacementOwnerRevalidatesThePendingGeneration) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(4U));
    gate.invalidate();
    gate.revalidate();
    EXPECT_EQ(gate.complete(4U, true, true), Disposition::ready);
    EXPECT_TRUE(gate.ready());
}

TEST(SingletonTaskPolicyGateTest, StaleCompletionCannotChangeCurrentGeneration) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(5U));
    EXPECT_EQ(gate.complete(4U, true, true), Disposition::stale);
    EXPECT_TRUE(gate.pending());
    EXPECT_FALSE(gate.ready());
    EXPECT_FALSE(gate.begin(6U));
    EXPECT_EQ(gate.complete(5U, true, true), Disposition::ready);
}

TEST(SingletonTaskPolicyGateTest, RearmCycleBlocksRemapUntilCurrentPublicationCompletes) {
    SingletonTaskPolicyGate gate;

    ASSERT_TRUE(gate.begin(6U));
    ASSERT_EQ(gate.complete(6U, true, true), Disposition::ready);
    ASSERT_TRUE(gate.ready());

    ASSERT_TRUE(gate.begin(7U));
    EXPECT_TRUE(gate.pending());
    EXPECT_FALSE(gate.ready());
    EXPECT_FALSE(gate.begin(8U));
    EXPECT_EQ(gate.complete(6U, true, true), Disposition::stale);
    EXPECT_FALSE(gate.ready());
    EXPECT_EQ(gate.complete(7U, true, true), Disposition::ready);
    EXPECT_TRUE(gate.ready());
}

} // namespace
} // namespace prismdrake::shell::runtime
