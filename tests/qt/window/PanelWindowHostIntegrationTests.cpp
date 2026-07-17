#include "PanelWindowHost.hpp"

#include "AtomCache.hpp"
#include "PropertyReader.hpp"
#include "X11Connection.hpp"

#include <QGuiApplication>
#include <QWindow>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace prismdrake::shell::window {
namespace {

class PanelWindowHostIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_FALSE(std::string_view{display_}.empty());
    }

    const char *display_ = nullptr;
};

TEST_F(PanelWindowHostIntegrationTest, MapsFixedBottomDockWithoutStartupFocusEligibility) {
    QWindow panel;
    bool connectionLost = false;
    auto created =
        PanelWindowHost::create(panel, display_, 48U, [&connectionLost](const foundation::Error &) {
            connectionLost = true;
        });

    ASSERT_TRUE(created) << created.error().message;
    auto host = std::move(created).value();
    EXPECT_TRUE(panel.isVisible());
    EXPECT_TRUE(panel.flags().testFlag(Qt::FramelessWindowHint));
    EXPECT_TRUE(panel.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_FALSE(host->keyboardAccessEnabled());
    EXPECT_FALSE(connectionLost);

    ASSERT_TRUE(host->placement());
    const auto &placement = *host->placement();
    EXPECT_EQ(panel.x(), static_cast<int>(placement.dock.panel.x));
    EXPECT_EQ(panel.y(), static_cast<int>(placement.dock.panel.y));
    EXPECT_EQ(panel.width(), static_cast<int>(placement.dock.panel.width));
    EXPECT_EQ(panel.height(), 48);

    auto inspector = x11::X11Connection::connect(display_);
    ASSERT_TRUE(inspector);
    const auto atoms = x11::AtomCache::create(inspector.value());
    ASSERT_TRUE(atoms);
    ASSERT_LE(panel.winId(), std::numeric_limits<x11::WindowId::Value>::max());
    const auto panelId =
        x11::WindowId::fromProtocol(static_cast<x11::WindowId::Value>(panel.winId()));
    ASSERT_TRUE(panelId);

    const auto read = [&inspector, &atoms](x11::WindowId target, x11::AtomName property,
                                           x11::AtomName type, std::size_t items) {
        return x11::PropertyReader::read(
            inspector.value(), atoms.value(), target, property,
            {type, x11::PropertyFormat::bits_32, items, items * sizeof(std::uint32_t)});
    };

    const auto dockType =
        read(panelId.value(), x11::AtomName::net_wm_window_type, x11::AtomName::atom, 1U);
    ASSERT_TRUE(dockType);
    const auto dockTypeItems = dockType.value().uint32Items();
    ASSERT_TRUE(dockTypeItems);
    ASSERT_TRUE(atoms.value().atom(x11::AtomName::net_wm_window_type_dock));
    EXPECT_EQ(dockTypeItems.value(),
              (std::vector<std::uint32_t>{
                  atoms.value().atom(x11::AtomName::net_wm_window_type_dock)->value()}));

    const auto strut = read(panelId.value(), x11::AtomName::net_wm_strut, x11::AtomName::cardinal,
                            placement.dock.strut.size());
    ASSERT_TRUE(strut);
    const auto strutItems = strut.value().uint32Items();
    ASSERT_TRUE(strutItems);
    EXPECT_EQ(strutItems.value(), (std::vector<std::uint32_t>(placement.dock.strut.begin(),
                                                              placement.dock.strut.end())));

    const auto strutPartial = read(panelId.value(), x11::AtomName::net_wm_strut_partial,
                                   x11::AtomName::cardinal, placement.dock.strutPartial.size());
    ASSERT_TRUE(strutPartial);
    const auto strutPartialItems = strutPartial.value().uint32Items();
    ASSERT_TRUE(strutPartialItems);
    EXPECT_EQ(strutPartialItems.value(),
              (std::vector<std::uint32_t>(placement.dock.strutPartial.begin(),
                                          placement.dock.strutPartial.end())));

    const auto active = read(inspector.value().screen().rootWindow,
                             x11::AtomName::net_active_window, x11::AtomName::window, 1U);
    if (active) {
        const auto activeItems = active.value().uint32Items();
        ASSERT_TRUE(activeItems);
        ASSERT_LE(activeItems.value().size(), 1U);
        EXPECT_TRUE(activeItems.value().empty() ||
                    activeItems.value().front() != panelId.value().value());
    } else {
        EXPECT_EQ(active.error().code, foundation::ErrorCode::not_found);
    }
}

TEST_F(PanelWindowHostIntegrationTest, EnablesFocusOnlyForDeliberateKeyboardAccess) {
    QWindow panel;
    auto created = PanelWindowHost::create(panel, display_, 40U, [](const foundation::Error &) {});
    ASSERT_TRUE(created) << created.error().message;
    auto host = std::move(created).value();

    ASSERT_TRUE(host->requestKeyboardAccess());
    EXPECT_TRUE(host->keyboardAccessEnabled());
    EXPECT_FALSE(panel.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_TRUE(panel.isVisible());

    ASSERT_TRUE(host->releaseKeyboardAccess());
    EXPECT_FALSE(host->keyboardAccessEnabled());
    EXPECT_TRUE(panel.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
    EXPECT_TRUE(panel.isVisible());
}

TEST_F(PanelWindowHostIntegrationTest, AppliesRuntimeHeightAsOneValidatedPlacement) {
    QWindow panel;
    auto created = PanelWindowHost::create(panel, display_, 40U, [](const foundation::Error &) {});
    ASSERT_TRUE(created) << created.error().message;
    auto host = std::move(created).value();
    ASSERT_TRUE(host->placement());
    const auto initial = host->placement();

    ASSERT_TRUE(host->setPanelHeight(72U));
    ASSERT_TRUE(host->placement());
    EXPECT_EQ(host->placement()->dock.panel.height, 72U);
    EXPECT_EQ(panel.height(), 72);
    EXPECT_EQ(panel.y(), static_cast<int>(host->placement()->dock.panel.y));

    EXPECT_FALSE(host->setPanelHeight(0U));
    ASSERT_TRUE(host->placement());
    EXPECT_EQ(host->placement()->dock.panel.height, 72U);
    EXPECT_NE(host->placement(), initial);
    EXPECT_EQ(panel.height(), 72);
}

} // namespace
} // namespace prismdrake::shell::window

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
