#include "DockProperties.hpp"

#include "AtomCache.hpp"
#include "PanelStrutGeometry.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t panelHeight = 48U;
constexpr std::uint32_t maximumObservedPropertyItems = 256U;
constexpr auto workareaApplicationTimeout = 8s;

struct ConnectionDeleter final {
    void operator()(xcb_connection_t *connection) const noexcept {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
    }
};

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

using ConnectionHandle = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;
using InternReply = std::unique_ptr<xcb_intern_atom_reply_t, FreeDeleter>;
using PropertyReply = std::unique_ptr<xcb_get_property_reply_t, FreeDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

struct PropertyObservation final {
    xcb_atom_t type;
    std::uint8_t format;
    std::vector<std::uint32_t> values;
};

[[nodiscard]] foundation::Result<BottomPanelStrut>
publishPanelProperties(X11Connection &connection, const AtomCache &atoms, WindowId panel,
                       RootGeometry root, OutputGeometry output) {
    return DockProperties::publishBottomPanel(connection, atoms, panel, root, output, panelHeight);
}

class DockPropertiesIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_FALSE(std::string_view{display_}.empty());

        int screenIndex = 0;
        connection_.reset(xcb_connect(display_, &screenIndex));
        ASSERT_TRUE(connection_);
        ASSERT_EQ(xcb_connection_has_error(connection_.get()), 0);

        const auto *setup = xcb_get_setup(connection_.get());
        ASSERT_NE(setup, nullptr);
        auto iterator = xcb_setup_roots_iterator(setup);
        for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
            xcb_screen_next(&iterator);
        }
        ASSERT_GT(iterator.rem, 0);
        ASSERT_NE(iterator.data, nullptr);
        screen_ = iterator.data;
        root_ = screen_->root;

        netWmWindowType_ = intern("_NET_WM_WINDOW_TYPE");
        netWmWindowTypeDock_ = intern("_NET_WM_WINDOW_TYPE_DOCK");
        netWmStrut_ = intern("_NET_WM_STRUT");
        netWmStrutPartial_ = intern("_NET_WM_STRUT_PARTIAL");
        netSupportingWmCheck_ = intern("_NET_SUPPORTING_WM_CHECK");
        netSupported_ = intern("_NET_SUPPORTED");
        netWorkarea_ = intern("_NET_WORKAREA");
        netNumberOfDesktops_ = intern("_NET_NUMBER_OF_DESKTOPS");
        netCurrentDesktop_ = intern("_NET_CURRENT_DESKTOP");
    }

    void TearDown() override {
        if (connection_ && panel_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(connection_.get(), panel_);
            (void)xcb_flush(connection_.get());
            panel_ = XCB_WINDOW_NONE;
        }
    }

    [[nodiscard]] xcb_atom_t intern(std::string_view name) {
        xcb_generic_error_t *rawError = nullptr;
        InternReply reply{xcb_intern_atom_reply(
            connection_.get(),
            xcb_intern_atom(connection_.get(), 0U, static_cast<std::uint16_t>(name.size()),
                            name.data()),
            &rawError)};
        ProtocolError error{rawError};
        EXPECT_FALSE(error);
        EXPECT_TRUE(reply);
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    [[nodiscard]] std::optional<PropertyObservation> observeProperty(xcb_window_t window,
                                                                     xcb_atom_t property) const {
        xcb_generic_error_t *rawError = nullptr;
        PropertyReply reply{xcb_get_property_reply(
            connection_.get(),
            xcb_get_property(connection_.get(), 0U, window, property, XCB_GET_PROPERTY_TYPE_ANY, 0U,
                             maximumObservedPropertyItems),
            &rawError)};
        ProtocolError error{rawError};
        if (error || !reply || reply->type == XCB_ATOM_NONE || reply->bytes_after != 0U ||
            reply->format != 32U) {
            return std::nullopt;
        }

        const int valueBytes = xcb_get_property_value_length(reply.get());
        if (valueBytes < 0 || valueBytes % static_cast<int>(sizeof(std::uint32_t)) != 0) {
            return std::nullopt;
        }
        std::vector<std::uint32_t> values(static_cast<std::size_t>(valueBytes) /
                                          sizeof(std::uint32_t));
        if (!values.empty()) {
            const void *value = xcb_get_property_value(reply.get());
            if (value == nullptr) {
                return std::nullopt;
            }
            std::memcpy(values.data(), value, static_cast<std::size_t>(valueBytes));
        }
        return PropertyObservation{reply->type, reply->format, std::move(values)};
    }

    void expectExactProperty(xcb_window_t window, xcb_atom_t property, xcb_atom_t type,
                             std::span<const std::uint32_t> expected) const {
        const auto observed = observeProperty(window, property);
        ASSERT_TRUE(observed);
        EXPECT_EQ(observed->type, type);
        EXPECT_EQ(observed->format, 32U);
        EXPECT_EQ(observed->values, (std::vector<std::uint32_t>{expected.begin(), expected.end()}));
    }

    [[nodiscard]] xcb_window_t createPanel(const BottomPanelGeometry &geometry) {
        if (geometry.x > static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max()) ||
            geometry.y > static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max()) ||
            geometry.width >
                static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) ||
            geometry.height >
                static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max())) {
            ADD_FAILURE() << "the isolated Xvfb panel geometry exceeds core X11 request fields";
            return XCB_WINDOW_NONE;
        }
        panel_ = xcb_generate_id(connection_.get());
        const auto cookie = xcb_create_window_checked(
            connection_.get(), screen_->root_depth, panel_, root_,
            static_cast<std::int16_t>(geometry.x), static_cast<std::int16_t>(geometry.y),
            static_cast<std::uint16_t>(geometry.width), static_cast<std::uint16_t>(geometry.height),
            0U, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, 0U, nullptr);
        ProtocolError error{xcb_request_check(connection_.get(), cookie)};
        EXPECT_FALSE(error);
        return error ? static_cast<xcb_window_t>(XCB_WINDOW_NONE) : panel_;
    }

    void mapPanel() {
        ProtocolError error{xcb_request_check(connection_.get(),
                                              xcb_map_window_checked(connection_.get(), panel_))};
        ASSERT_FALSE(error);
        ASSERT_GT(xcb_flush(connection_.get()), 0);
    }

    [[nodiscard]] bool verifiedWindowManagerContracts() const {
        const auto rootOwner = observeProperty(root_, netSupportingWmCheck_);
        if (!rootOwner || rootOwner->type != XCB_ATOM_WINDOW || rootOwner->values.size() != 1U ||
            rootOwner->values.front() == XCB_WINDOW_NONE) {
            return false;
        }
        const auto owner = static_cast<xcb_window_t>(rootOwner->values.front());
        const auto selfReference = observeProperty(owner, netSupportingWmCheck_);
        if (!selfReference || selfReference->type != XCB_ATOM_WINDOW ||
            selfReference->values != rootOwner->values) {
            return false;
        }
        const auto supported = observeProperty(root_, netSupported_);
        if (!supported || supported->type != XCB_ATOM_ATOM) {
            return false;
        }
        const std::array required{netWmWindowType_, netWmWindowTypeDock_, netWmStrut_,
                                  netWmStrutPartial_, netWorkarea_};
        return std::ranges::all_of(required, [&supported](xcb_atom_t atom) {
            return std::ranges::find(supported->values, atom) != supported->values.end();
        });
    }

    template <typename Predicate>
    [[nodiscard]] bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return predicate();
    }

    [[nodiscard]] std::optional<std::array<std::uint32_t, 4U>> currentWorkarea() const {
        const auto desktopCount = observeProperty(root_, netNumberOfDesktops_);
        const auto currentDesktop = observeProperty(root_, netCurrentDesktop_);
        const auto workareas = observeProperty(root_, netWorkarea_);
        if (!desktopCount || desktopCount->type != XCB_ATOM_CARDINAL ||
            desktopCount->values.size() != 1U || desktopCount->values.front() == 0U ||
            desktopCount->values.front() > 32U || !currentDesktop ||
            currentDesktop->type != XCB_ATOM_CARDINAL || currentDesktop->values.size() != 1U ||
            currentDesktop->values.front() >= desktopCount->values.front() || !workareas ||
            workareas->type != XCB_ATOM_CARDINAL ||
            workareas->values.size() != desktopCount->values.front() * 4U) {
            return std::nullopt;
        }
        const auto offset = static_cast<std::size_t>(currentDesktop->values.front()) * 4U;
        return std::array<std::uint32_t, 4U>{
            workareas->values[offset], workareas->values[offset + 1U],
            workareas->values[offset + 2U], workareas->values[offset + 3U]};
    }

    [[nodiscard]] foundation::Result<RootGeometry> fullRootGeometry() const {
        return RootGeometry::create(screen_->width_in_pixels, screen_->height_in_pixels);
    }

    [[nodiscard]] foundation::Result<OutputGeometry> fullRootOutput(RootGeometry root) const {
        return OutputGeometry::create(root, 0, 0, root.widthPx(), root.heightPx());
    }

    [[nodiscard]] foundation::Result<BottomPanelStrut>
    fullRootReservation(RootGeometry root, OutputGeometry output) const {
        return calculateBottomPanelStrut(root, output, panelHeight);
    }

    [[nodiscard]] foundation::Result<BottomPanelStrut> fullRootReservation() const {
        const auto root = fullRootGeometry();
        if (!root) {
            return foundation::Result<BottomPanelStrut>::failure(root.error());
        }
        const auto output = fullRootOutput(root.value());
        if (!output) {
            return foundation::Result<BottomPanelStrut>::failure(output.error());
        }
        return fullRootReservation(root.value(), output.value());
    }

    const char *display_ = nullptr;
    ConnectionHandle connection_;
    const xcb_screen_t *screen_ = nullptr;
    xcb_window_t root_ = XCB_WINDOW_NONE;
    xcb_window_t panel_ = XCB_WINDOW_NONE;
    xcb_atom_t netWmWindowType_ = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowTypeDock_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStrut_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStrutPartial_ = XCB_ATOM_NONE;
    xcb_atom_t netSupportingWmCheck_ = XCB_ATOM_NONE;
    xcb_atom_t netSupported_ = XCB_ATOM_NONE;
    xcb_atom_t netWorkarea_ = XCB_ATOM_NONE;
    xcb_atom_t netNumberOfDesktops_ = XCB_ATOM_NONE;
    xcb_atom_t netCurrentDesktop_ = XCB_ATOM_NONE;
};

TEST_F(DockPropertiesIntegrationTest, PublishesExactStandardPanelProperties) {
    const auto root = fullRootGeometry();
    ASSERT_TRUE(root);
    const auto output = fullRootOutput(root.value());
    ASSERT_TRUE(output);
    const auto reservation = fullRootReservation();
    ASSERT_TRUE(reservation);
    const auto panel = createPanel(reservation.value().panel);
    ASSERT_NE(panel, XCB_WINDOW_NONE);
    const auto panelId = WindowId::fromProtocol(panel);
    ASSERT_TRUE(panelId);

    auto publishingConnection = X11Connection::connect(display_);
    ASSERT_TRUE(publishingConnection);
    const auto atoms = AtomCache::create(publishingConnection.value());
    ASSERT_TRUE(atoms);
    const auto published = publishPanelProperties(publishingConnection.value(), atoms.value(),
                                                  panelId.value(), root.value(), output.value());
    ASSERT_TRUE(published);
    EXPECT_EQ(published.value(), reservation.value());
    mapPanel();

    const std::array<std::uint32_t, 1U> dockType{netWmWindowTypeDock_};
    expectExactProperty(panel, netWmWindowType_, XCB_ATOM_ATOM, dockType);
    expectExactProperty(panel, netWmStrut_, XCB_ATOM_CARDINAL, reservation.value().strut);
    expectExactProperty(panel, netWmStrutPartial_, XCB_ATOM_CARDINAL,
                        reservation.value().strutPartial);
}

TEST_F(DockPropertiesIntegrationTest, RejectsMovedConnectionWithoutDereferencingIt) {
    const auto root = fullRootGeometry();
    ASSERT_TRUE(root);
    const auto output = fullRootOutput(root.value());
    ASSERT_TRUE(output);
    const auto reservation = fullRootReservation(root.value(), output.value());
    ASSERT_TRUE(reservation);
    const auto panel = createPanel(reservation.value().panel);
    ASSERT_NE(panel, XCB_WINDOW_NONE);
    const auto panelId = WindowId::fromProtocol(panel);
    ASSERT_TRUE(panelId);

    auto source = X11Connection::connect(display_);
    ASSERT_TRUE(source);
    const auto atoms = AtomCache::create(source.value());
    ASSERT_TRUE(atoms);
    X11Connection retained{std::move(source.value())};

    const auto published = publishPanelProperties(source.value(), atoms.value(), panelId.value(),
                                                  root.value(), output.value());
    const auto removed = DockProperties::remove(source.value(), atoms.value(), panelId.value());

    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, foundation::ErrorCode::invalid_argument);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, foundation::ErrorCode::invalid_argument);
    EXPECT_TRUE(retained.healthy());
}

TEST_F(DockPropertiesIntegrationTest, VerifiedWindowManagerAppliesCurrentDesktopWorkarea) {
    const bool windowManagerExpected =
        std::string_view{std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr
                             ? std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM")
                             : ""} == "1";
    const bool ready = windowManagerExpected
                           ? waitUntil([this] { return verifiedWindowManagerContracts(); }, 2s)
                           : verifiedWindowManagerContracts();
    if (!ready && !windowManagerExpected) {
        GTEST_SKIP() << "run this case through the isolated Xvfb/Openbox lane";
    }
    ASSERT_TRUE(ready) << "the expected WM did not publish a verified EWMH owner and contracts";

    const auto root = fullRootGeometry();
    ASSERT_TRUE(root);
    const auto output = fullRootOutput(root.value());
    ASSERT_TRUE(output);
    const auto reservation = fullRootReservation(root.value(), output.value());
    ASSERT_TRUE(reservation);
    const auto panel = createPanel(reservation.value().panel);
    ASSERT_NE(panel, XCB_WINDOW_NONE);
    const auto panelId = WindowId::fromProtocol(panel);
    ASSERT_TRUE(panelId);

    auto publishingConnection = X11Connection::connect(display_);
    ASSERT_TRUE(publishingConnection);
    const auto atoms = AtomCache::create(publishingConnection.value());
    ASSERT_TRUE(atoms);
    const auto published = publishPanelProperties(publishingConnection.value(), atoms.value(),
                                                  panelId.value(), root.value(), output.value());
    ASSERT_TRUE(published);
    EXPECT_EQ(published.value(), reservation.value());
    mapPanel();

    const std::array<std::uint32_t, 4U> expected{0U, 0U, screen_->width_in_pixels,
                                                 screen_->height_in_pixels - panelHeight};
    const bool appliedExpectedWorkarea = waitUntil(
        [this, &expected] {
            const auto workarea = currentWorkarea();
            return workarea && workarea.value() == expected;
        },
        workareaApplicationTimeout);
    const auto applied = currentWorkarea();
    ASSERT_TRUE(applied);
    EXPECT_TRUE(appliedExpectedWorkarea);
    EXPECT_EQ(applied.value(), expected);
}

} // namespace
} // namespace prismdrake::x11
