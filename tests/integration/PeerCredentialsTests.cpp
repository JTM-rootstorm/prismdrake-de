#include "PeerCredentials.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

using prismdrake::ipc::EffectiveUserId;
using prismdrake::ipc::isSameEffectiveUser;
using prismdrake::ipc::isSameUserPeer;

TEST(PeerCredentialsTest, AcceptsMatchingEffectiveUsers) {
    constexpr EffectiveUserId userId = 1000;
    EXPECT_TRUE(isSameEffectiveUser(userId, userId));
}

TEST(PeerCredentialsTest, RejectsMissingEffectiveUser) {
    EXPECT_FALSE(isSameEffectiveUser(std::nullopt, 1000));
    EXPECT_FALSE(isSameUserPeer(nullptr));
}

TEST(PeerCredentialsTest, RejectsMismatchedEffectiveUsers) {
    EXPECT_FALSE(isSameEffectiveUser(1000, 1001));
}

} // namespace
