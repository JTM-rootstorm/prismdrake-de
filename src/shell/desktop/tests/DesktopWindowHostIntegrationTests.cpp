#include "DesktopWindowHost.hpp"

#include "AtomCache.hpp"
#include "DesktopWallpaperItem.hpp"
#include "PropertyReader.hpp"
#include "TestTopology.hpp"
#include "X11Connection.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace prismdrake::shell::desktop {
namespace {

using namespace std::chrono_literals;

class DesktopWindowHostIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_NE(QGuiApplication::primaryScreen(), nullptr);
    }

    [[nodiscard]] std::unique_ptr<DesktopWindowHost> host() const {
        auto created = DesktopWindowHost::create(
            display_, [](std::string_view) { return QGuiApplication::primaryScreen(); });
        EXPECT_TRUE(created) << created.error().message;
        return created ? std::move(created).value() : nullptr;
    }

    const char *display_{nullptr};
};

TEST_F(DesktopWindowHostIntegrationTest, MapsTwoTypedInputTransparentDesktopsAndReconcilesRemoval) {
    test::RegistryHarness registry;
    DesktopSurfaceController controller;
    const std::array twoScales{OutputScaleObservation{"LEFT-1", 1.0},
                               OutputScaleObservation{"RIGHT-1", 1.0}};
    auto update = controller.prepare(registry.observe(1U), twoScales, test::imageWallpaper(1U));
    ASSERT_TRUE(update);
    auto desktopHost = host();
    ASSERT_NE(desktopHost, nullptr);

    ASSERT_TRUE(desktopHost->apply(update.value()));
    controller.commit(std::move(update).value());
    ASSERT_EQ(desktopHost->windowCount(), 2U);
    auto *left = desktopHost->window("LEFT-1");
    auto *right = desktopHost->window("RIGHT-1");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->geometry(), QRect(0, 0, 512, 768));
    EXPECT_EQ(right->geometry(), QRect(512, 0, 512, 768));
    for (const auto *window : {left, right}) {
        EXPECT_TRUE(window->isVisible());
        EXPECT_TRUE(window->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        EXPECT_TRUE(window->flags().testFlag(Qt::WindowTransparentForInput));
        auto *renderer =
            window->findChild<DesktopWallpaperItem *>(QStringLiteral("prismdrakeDesktopWallpaper"));
        ASSERT_NE(renderer, nullptr);
        EXPECT_EQ(renderer->wallpaperGeneration(), 1U);
        EXPECT_TRUE(renderer->paintsImage());
    }

    auto inspector = x11::X11Connection::connect(display_);
    ASSERT_TRUE(inspector);
    auto atoms = x11::AtomCache::create(inspector.value());
    ASSERT_TRUE(atoms);
    for (const auto *window : {left, right}) {
        ASSERT_LE(window->winId(), std::numeric_limits<x11::WindowId::Value>::max());
        auto id = x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(window->winId()));
        ASSERT_TRUE(id);
        auto property = x11::PropertyReader::read(
            inspector.value(), atoms.value(), id.value(), x11::AtomName::net_wm_window_type,
            {x11::AtomName::atom, x11::PropertyFormat::bits_32, 1U, sizeof(std::uint32_t)});
        ASSERT_TRUE(property);
        auto values = property.value().uint32Items();
        ASSERT_TRUE(values);
        ASSERT_TRUE(atoms.value().atom(x11::AtomName::net_wm_window_type_desktop));
        EXPECT_EQ(values.value(),
                  (std::vector<std::uint32_t>{
                      atoms.value().atom(x11::AtomName::net_wm_window_type_desktop)->value()}));
    }

    const auto leftId = left->winId();
    const std::array oneScale{OutputScaleObservation{"LEFT-1", 1.0}};
    auto removed = controller.prepare(registry.observe(2U, false), oneScale);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(desktopHost->apply(removed.value()));
    controller.commit(std::move(removed).value());
    EXPECT_EQ(desktopHost->windowCount(), 1U);
    ASSERT_NE(desktopHost->window("LEFT-1"), nullptr);
    EXPECT_EQ(desktopHost->window("LEFT-1")->winId(), leftId);
    EXPECT_EQ(desktopHost->window("RIGHT-1"), nullptr);
}

TEST_F(DesktopWindowHostIntegrationTest, OpenboxLeavesFocusAndStackingWithOrdinaryClient) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") == nullptr) {
        GTEST_SKIP() << "Openbox-only standards integration";
    }
    test::RegistryHarness registry;
    DesktopSurfaceController controller;
    const std::array scales{OutputScaleObservation{"LEFT-1", 1.0},
                            OutputScaleObservation{"RIGHT-1", 1.0}};
    auto update = controller.prepare(registry.observe(1U), scales, test::solidWallpaper(1U));
    ASSERT_TRUE(update);
    auto desktopHost = host();
    ASSERT_NE(desktopHost, nullptr);
    ASSERT_TRUE(desktopHost->apply(update.value()));

    QWindow ordinary;
    ordinary.setTitle(QStringLiteral("Prismdrake desktop stacking fixture"));
    ordinary.setGeometry(200, 200, 320, 240);
    ordinary.show();
    ordinary.requestActivate();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!ordinary.isActive() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(ordinary.isActive());
    EXPECT_FALSE(desktopHost->window("LEFT-1")->isActive());
    EXPECT_FALSE(desktopHost->window("RIGHT-1")->isActive());
}

} // namespace
} // namespace prismdrake::shell::desktop

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
