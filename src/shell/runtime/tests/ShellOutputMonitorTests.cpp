#include "ShellOutputMonitor.hpp"

#include "OutputTopology.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace prismdrake::shell::runtime {
namespace {

[[nodiscard]] x11::OutputId outputId(std::uint32_t value) {
    return x11::OutputId::fromProtocol(value).value();
}

[[nodiscard]] x11::CrtcId crtcId(std::uint32_t value) {
    return x11::CrtcId::fromProtocol(value).value();
}

[[nodiscard]] x11::OutputTopologySnapshot topology() {
    const auto root = x11::RootGeometry::create(1600U, 600U).value();
    std::vector<x11::OutputCandidate> outputs{
        {outputId(1U), crtcId(10U), 0, 0, 800U, 600U, "LEFT-1"},
        {outputId(2U), crtcId(20U), 800, 0, 800U, 600U, "RIGHT-1"},
    };
    return x11::buildOutputTopology(
               {true, root, outputs.size(), outputs.size(), 1U, std::move(outputs), outputId(1U)})
        .value();
}

TEST(ShellOutputMonitorTest, ObserverFailureKeepsTopologyAndPolicyRetryable) {
    bool reject = true;
    std::vector<outputs::SurfaceRegistrySnapshot> accepted;
    ShellOutputMonitor monitor{
        ":test",
        [&](const outputs::SurfaceRegistrySnapshot &snapshot) {
            if (reject) {
                return foundation::Result<void>::failure(
                    {foundation::ErrorCode::cancelled, "injected presentation failure", "retry"});
            }
            accepted.push_back(snapshot);
            return foundation::Result<void>::success();
        },
        {}};
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::primary, {}}, 10U));
    const std::array screens{
        outputs::QtScreenObservation{"LEFT-1", {0, 0, 800U, 600U}, 1.0},
        outputs::QtScreenObservation{"RIGHT-1", {800, 0, 800U, 600U}, 1.0},
    };

    EXPECT_FALSE(monitor.observeTopology(topology(), screens));
    reject = false;
    ASSERT_TRUE(monitor.observeTopology(topology(), screens));
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.back().surfaceGeneration(), 1U);
    ASSERT_EQ(accepted.back().panels().size(), 1U);

    reject = true;
    EXPECT_FALSE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 11U));
    reject = false;
    ASSERT_TRUE(monitor.configure(48U, {outputs::PanelOutputPolicy::all, {}}, 11U));
    ASSERT_EQ(accepted.size(), 2U);
    EXPECT_EQ(accepted.back().topologyGeneration(), 1U);
    EXPECT_EQ(accepted.back().settingsGeneration(), 11U);
    EXPECT_EQ(accepted.back().surfaceGeneration(), 2U);
    EXPECT_EQ(accepted.back().panels().size(), 2U);
}

} // namespace
} // namespace prismdrake::shell::runtime
