#include "ShellControlBusOwner.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace prismdrake::shell::runtime {
namespace {

class PresentationPort final : public control::ShellPresentationPort {
  public:
    [[nodiscard]] control::ShellRequestFailure
    request(control::ShellAction, std::string_view,
            foundation::MonotonicClock::TimePoint) override {
        return control::ShellRequestFailure::none;
    }
};

TEST(ShellControlBusOwnerTest, StopMakesTheOneShotOwnerFailClosedBeforeAnyStart) {
    PresentationPort presentation;
    ShellControlBusOwner owner{presentation};

    owner.stop();

    EXPECT_FALSE(owner.running());
    const auto restarted = owner.start();
    ASSERT_FALSE(restarted);
    EXPECT_EQ(restarted.error().code, foundation::ErrorCode::invalid_argument);
}

} // namespace
} // namespace prismdrake::shell::runtime
