#include "EwmhWindowRequests.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include <poll.h>
#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

struct ConnectionDeleter final {
    void operator()(xcb_connection_t *connection) const noexcept {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
    }
};

using Connection = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;
using InternReply = std::unique_ptr<xcb_intern_atom_reply_t, FreeDeleter>;
using GenericEvent = std::unique_ptr<xcb_generic_event_t, FreeDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

class EwmhWindowRequestsIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_FALSE(std::string_view{display_}.empty());

        int screenIndex = 0;
        wm_.reset(xcb_connect(display_, &screenIndex));
        ASSERT_TRUE(wm_);
        ASSERT_EQ(xcb_connection_has_error(wm_.get()), 0);
        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(wm_.get()));
        for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
            xcb_screen_next(&iterator);
        }
        ASSERT_GT(iterator.rem, 0);
        ASSERT_NE(iterator.data, nullptr);
        root_ = iterator.data->root;

        supportingCheck_ = intern("_NET_SUPPORTING_WM_CHECK");
        netSupported_ = intern("_NET_SUPPORTED");
        netActiveWindow_ = intern("_NET_ACTIVE_WINDOW");
        netCloseWindow_ = intern("_NET_CLOSE_WINDOW");
        wmChangeState_ = intern("WM_CHANGE_STATE");
        supportingWindow_ = createWindow();
        targetWindow_ = createWindow();

        const std::uint32_t eventMask =
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
        check(
            xcb_change_window_attributes_checked(wm_.get(), root_, XCB_CW_EVENT_MASK, &eventMask));
        setWindow(root_, supportingCheck_, supportingWindow_);
        setWindow(supportingWindow_, supportingCheck_, supportingWindow_);
        const std::array supported{netActiveWindow_, netCloseWindow_};
        check(xcb_change_property_checked(wm_.get(), XCB_PROP_MODE_REPLACE, root_, netSupported_,
                                          XCB_ATOM_ATOM, 32U, supported.size(), supported.data()));
    }

    void TearDown() override {
        if (!wm_) {
            return;
        }
        if (root_ != XCB_WINDOW_NONE) {
            (void)xcb_delete_property(wm_.get(), root_, supportingCheck_);
            (void)xcb_delete_property(wm_.get(), root_, netSupported_);
            const std::uint32_t noEvents = 0U;
            (void)xcb_change_window_attributes(wm_.get(), root_, XCB_CW_EVENT_MASK, &noEvents);
        }
        if (targetWindow_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(wm_.get(), targetWindow_);
        }
        if (supportingWindow_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(wm_.get(), supportingWindow_);
        }
        (void)xcb_flush(wm_.get());
    }

    [[nodiscard]] xcb_atom_t intern(std::string_view name) {
        xcb_generic_error_t *rawError = nullptr;
        InternReply reply{xcb_intern_atom_reply(
            wm_.get(),
            xcb_intern_atom(wm_.get(), 0U, static_cast<std::uint16_t>(name.size()), name.data()),
            &rawError)};
        ProtocolError error{rawError};
        EXPECT_FALSE(error);
        EXPECT_TRUE(reply);
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    [[nodiscard]] xcb_window_t createWindow() {
        const auto window = xcb_generate_id(wm_.get());
        check(xcb_create_window_checked(wm_.get(), XCB_COPY_FROM_PARENT, window, root_, 0, 0, 1, 1,
                                        0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0U,
                                        nullptr));
        return window;
    }

    void setWindow(xcb_window_t target, xcb_atom_t property, xcb_window_t value) {
        check(xcb_change_property_checked(wm_.get(), XCB_PROP_MODE_REPLACE, target, property,
                                          XCB_ATOM_WINDOW, 32U, 1U, &value));
    }

    void check(xcb_void_cookie_t cookie) {
        ProtocolError error{xcb_request_check(wm_.get(), cookie)};
        ASSERT_FALSE(error);
        ASSERT_EQ(xcb_connection_has_error(wm_.get()), 0);
    }

    [[nodiscard]] std::optional<xcb_client_message_event_t> nextClientMessage() {
        for (std::size_t attempt = 0U; attempt < 16U; ++attempt) {
            GenericEvent event{xcb_poll_for_event(wm_.get())};
            if (!event) {
                pollfd descriptor{xcb_get_file_descriptor(wm_.get()), POLLIN, 0};
                if (poll(&descriptor, 1, 1000) <= 0) {
                    return std::nullopt;
                }
                event.reset(xcb_poll_for_event(wm_.get()));
            }
            if (!event) {
                return std::nullopt;
            }
            if ((event->response_type & 0x7fU) == XCB_CLIENT_MESSAGE) {
                xcb_client_message_event_t copied{};
                static_assert(sizeof(copied) <= sizeof(*event));
                std::memcpy(&copied, event.get(), sizeof(copied));
                return copied;
            }
        }
        return std::nullopt;
    }

    const char *display_ = nullptr;
    Connection wm_;
    xcb_window_t root_ = XCB_WINDOW_NONE;
    xcb_window_t supportingWindow_ = XCB_WINDOW_NONE;
    xcb_window_t targetWindow_ = XCB_WINDOW_NONE;
    xcb_atom_t supportingCheck_ = XCB_ATOM_NONE;
    xcb_atom_t netSupported_ = XCB_ATOM_NONE;
    xcb_atom_t netActiveWindow_ = XCB_ATOM_NONE;
    xcb_atom_t netCloseWindow_ = XCB_ATOM_NONE;
    xcb_atom_t wmChangeState_ = XCB_ATOM_NONE;
};

TEST_F(EwmhWindowRequestsIntegrationTest, SendsCheckedRequestsAndRejectsDestroyedTarget) {
    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto requests = EwmhWindowRequests::create(connection.value());
    ASSERT_TRUE(requests);
    ASSERT_TRUE(requests.value().supportsActivation());
    ASSERT_TRUE(requests.value().supportsMinimize());
    ASSERT_TRUE(requests.value().supportsClose());
    const auto target = WindowId::fromProtocol(targetWindow_).value();

    ASSERT_TRUE(requests.value().activate(connection.value(), target, 123U));
    const auto activate = nextClientMessage();
    ASSERT_TRUE(activate);
    EXPECT_EQ(activate->format, 32U);
    EXPECT_EQ(activate->window, targetWindow_);
    EXPECT_EQ(activate->type, netActiveWindow_);
    EXPECT_EQ(activate->data.data32[0], 2U);
    EXPECT_EQ(activate->data.data32[1], 123U);

    ASSERT_TRUE(requests.value().minimize(connection.value(), target));
    const auto minimize = nextClientMessage();
    ASSERT_TRUE(minimize);
    EXPECT_EQ(minimize->format, 32U);
    EXPECT_EQ(minimize->window, targetWindow_);
    EXPECT_EQ(minimize->type, wmChangeState_);
    EXPECT_EQ(minimize->data.data32[0], 3U);

    ASSERT_TRUE(requests.value().close(connection.value(), target, 456U));
    const auto close = nextClientMessage();
    ASSERT_TRUE(close);
    EXPECT_EQ(close->format, 32U);
    EXPECT_EQ(close->window, targetWindow_);
    EXPECT_EQ(close->type, netCloseWindow_);
    EXPECT_EQ(close->data.data32[0], 456U);
    EXPECT_EQ(close->data.data32[1], 2U);

    check(xcb_destroy_window_checked(wm_.get(), targetWindow_));
    targetWindow_ = XCB_WINDOW_NONE;
    const auto stale = requests.value().minimize(connection.value(), target);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, foundation::ErrorCode::not_found);
    EXPECT_FALSE(nextClientMessage());
}

TEST_F(EwmhWindowRequestsIntegrationTest, RejectsRootTargetsAndCrossConnectionUse) {
    auto connection = X11Connection::connect(display_);
    auto otherConnection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    ASSERT_TRUE(otherConnection);
    auto requests = EwmhWindowRequests::create(connection.value());
    ASSERT_TRUE(requests);
    const auto target = WindowId::fromProtocol(targetWindow_).value();

    const auto rootTarget =
        requests.value().minimize(connection.value(), connection.value().screen().rootWindow);
    const auto rootCurrent = requests.value().activate(connection.value(), target, 1U,
                                                       connection.value().screen().rootWindow);
    const auto wrongConnection = requests.value().close(otherConnection.value(), target, 1U);

    ASSERT_FALSE(rootTarget);
    EXPECT_EQ(rootTarget.error().code, foundation::ErrorCode::invalid_argument);
    ASSERT_FALSE(rootCurrent);
    EXPECT_EQ(rootCurrent.error().code, foundation::ErrorCode::invalid_argument);
    ASSERT_FALSE(wrongConnection);
    EXPECT_EQ(wrongConnection.error().code, foundation::ErrorCode::invalid_argument);
    EXPECT_FALSE(nextClientMessage());
}

TEST_F(EwmhWindowRequestsIntegrationTest, MissingVerifiedOwnerDisablesEveryRequest) {
    check(xcb_delete_property_checked(wm_.get(), root_, supportingCheck_));
    check(xcb_delete_property_checked(wm_.get(), root_, netSupported_));

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    const auto requests = EwmhWindowRequests::create(connection.value());

    ASSERT_TRUE(requests);
    EXPECT_FALSE(requests.value().supportsActivation());
    EXPECT_FALSE(requests.value().supportsMinimize());
    EXPECT_FALSE(requests.value().supportsClose());
    const auto target = WindowId::fromProtocol(targetWindow_).value();
    const auto activate = requests.value().activate(connection.value(), target, 1U);
    const auto minimize = requests.value().minimize(connection.value(), target);
    const auto close = requests.value().close(connection.value(), target, 1U);
    ASSERT_FALSE(activate);
    EXPECT_EQ(activate.error().code, foundation::ErrorCode::unsupported);
    ASSERT_FALSE(minimize);
    EXPECT_EQ(minimize.error().code, foundation::ErrorCode::unsupported);
    ASSERT_FALSE(close);
    EXPECT_EQ(close.error().code, foundation::ErrorCode::unsupported);
    EXPECT_FALSE(nextClientMessage());
}

} // namespace
} // namespace prismdrake::x11
