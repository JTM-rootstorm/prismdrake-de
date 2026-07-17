#include "EwmhCapabilities.hpp"
#include "EwmhCapabilitiesInternal.hpp"

#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

using InternReply = std::unique_ptr<xcb_intern_atom_reply_t, FreeDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

class EwmhCapabilitiesIntegrationTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        const char *display = std::getenv("DISPLAY");
        ASSERT_NE(display, nullptr);
        int screenIndex = 0;
        serverAnchor_ = xcb_connect(display, &screenIndex);
        ASSERT_NE(serverAnchor_, nullptr);
        ASSERT_EQ(xcb_connection_has_error(serverAnchor_), 0);
    }

    static void TearDownTestSuite() {
        if (serverAnchor_ != nullptr) {
            xcb_disconnect(serverAnchor_);
            serverAnchor_ = nullptr;
        }
    }

    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_FALSE(std::string_view{display_}.empty());

        int screenIndex = 0;
        connection_ = xcb_connect(display_, &screenIndex);
        ASSERT_NE(connection_, nullptr);
        ASSERT_EQ(xcb_connection_has_error(connection_), 0);

        const auto *setup = xcb_get_setup(connection_);
        ASSERT_NE(setup, nullptr);
        auto iterator = xcb_setup_roots_iterator(setup);
        for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
            xcb_screen_next(&iterator);
        }
        ASSERT_GT(iterator.rem, 0);
        ASSERT_NE(iterator.data, nullptr);
        root_ = iterator.data->root;

        supportingCheck_ = intern("_NET_SUPPORTING_WM_CHECK");
        netSupported_ = intern("_NET_SUPPORTED");
        netClientList_ = intern("_NET_CLIENT_LIST");
        netActiveWindow_ = intern("_NET_ACTIVE_WINDOW");
        netCloseWindow_ = intern("_NET_CLOSE_WINDOW");
        netNumberOfDesktops_ = intern("_NET_NUMBER_OF_DESKTOPS");
        netCurrentDesktop_ = intern("_NET_CURRENT_DESKTOP");
        netWmDesktop_ = intern("_NET_WM_DESKTOP");
        netWmWindowType_ = intern("_NET_WM_WINDOW_TYPE");
        netWmWindowTypeDock_ = intern("_NET_WM_WINDOW_TYPE_DOCK");
        netWmStrutPartial_ = intern("_NET_WM_STRUT_PARTIAL");

        deleteProperty(root_, supportingCheck_);
        deleteProperty(root_, netSupported_);
        flushChecked();
    }

    void TearDown() override {
        if (connection_ == nullptr) {
            return;
        }
        deleteProperty(root_, supportingCheck_);
        deleteProperty(root_, netSupported_);
        if (supportingWindow_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(connection_, supportingWindow_);
        }
        if (replacementWindow_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(connection_, replacementWindow_);
        }
        (void)xcb_flush(connection_);
        xcb_disconnect(connection_);
        connection_ = nullptr;
    }

    [[nodiscard]] xcb_atom_t intern(std::string_view name) {
        xcb_generic_error_t *rawError = nullptr;
        InternReply reply{xcb_intern_atom_reply(
            connection_,
            xcb_intern_atom(connection_, 0, static_cast<std::uint16_t>(name.size()), name.data()),
            &rawError)};
        ProtocolError error{rawError};
        EXPECT_FALSE(error);
        EXPECT_TRUE(reply);
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    void deleteProperty(xcb_window_t window, xcb_atom_t property) {
        if (window != XCB_WINDOW_NONE && property != XCB_ATOM_NONE) {
            (void)xcb_delete_property(connection_, window, property);
        }
    }

    void flushChecked() {
        ASSERT_GT(xcb_flush(connection_), 0);
        ASSERT_EQ(xcb_connection_has_error(connection_), 0);
    }

    [[nodiscard]] xcb_window_t createWindow() {
        const auto window = xcb_generate_id(connection_);
        const auto cookie = xcb_create_window_checked(
            connection_, XCB_COPY_FROM_PARENT, window, root_, 0, 0, 1, 1, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
        ProtocolError error{xcb_request_check(connection_, cookie)};
        EXPECT_FALSE(error);
        EXPECT_NE(window, XCB_WINDOW_NONE);
        if (error) {
            return static_cast<xcb_window_t>(XCB_WINDOW_NONE);
        }
        return window;
    }

    void createSupportingWindow() {
        ASSERT_EQ(supportingWindow_, XCB_WINDOW_NONE);
        supportingWindow_ = createWindow();
        ASSERT_NE(supportingWindow_, XCB_WINDOW_NONE);
    }

    void setWindowProperty(xcb_window_t target, xcb_atom_t property, xcb_window_t value) {
        const auto cookie = xcb_change_property_checked(connection_, XCB_PROP_MODE_REPLACE, target,
                                                        property, XCB_ATOM_WINDOW, 32, 1, &value);
        ProtocolError error{xcb_request_check(connection_, cookie)};
        ASSERT_FALSE(error);
    }

    void setSupportedAtoms(std::span<const xcb_atom_t> atoms, xcb_atom_t type = XCB_ATOM_ATOM) {
        const auto cookie = xcb_change_property_checked(connection_, XCB_PROP_MODE_REPLACE, root_,
                                                        netSupported_, type, 32, atoms.size(),
                                                        atoms.empty() ? nullptr : atoms.data());
        ProtocolError error{xcb_request_check(connection_, cookie)};
        ASSERT_FALSE(error);
    }

    void deleteRootOwnerChecked() {
        const auto cookie = xcb_delete_property_checked(connection_, root_, supportingCheck_);
        ProtocolError error{xcb_request_check(connection_, cookie)};
        ASSERT_FALSE(error);
    }

    static void replaceOwnerDuringDiscovery(void *context) {
        auto &fixture = *static_cast<EwmhCapabilitiesIntegrationTest *>(context);
        fixture.setWindowProperty(fixture.root_, fixture.supportingCheck_,
                                  fixture.replacementWindow_);
        fixture.flushChecked();
    }

    static void removeOwnerDuringDiscovery(void *context) {
        auto &fixture = *static_cast<EwmhCapabilitiesIntegrationTest *>(context);
        fixture.deleteRootOwnerChecked();
        fixture.flushChecked();
    }

    void configureVerifiedOwner(std::span<const xcb_atom_t> supported) {
        createSupportingWindow();
        setWindowProperty(root_, supportingCheck_, supportingWindow_);
        setWindowProperty(supportingWindow_, supportingCheck_, supportingWindow_);
        setSupportedAtoms(supported);
        flushChecked();
    }

    [[nodiscard]] foundation::Result<EwmhCapabilities>
    discover(detail::EwmhDiscoveryInterleaveHook interleave = nullptr,
             void *context = nullptr) const {
        auto connection = X11Connection::connect(display_);
        if (!connection) {
            return foundation::Result<EwmhCapabilities>::failure(connection.error());
        }
        return detail::discoverEwmhCapabilitiesWithInterleave(connection.value(), interleave,
                                                              context);
    }

    const char *display_ = nullptr;
    xcb_connection_t *connection_ = nullptr;
    xcb_window_t root_ = XCB_WINDOW_NONE;
    xcb_window_t supportingWindow_ = XCB_WINDOW_NONE;
    xcb_window_t replacementWindow_ = XCB_WINDOW_NONE;
    xcb_atom_t supportingCheck_ = XCB_ATOM_NONE;
    xcb_atom_t netSupported_ = XCB_ATOM_NONE;
    xcb_atom_t netClientList_ = XCB_ATOM_NONE;
    xcb_atom_t netActiveWindow_ = XCB_ATOM_NONE;
    xcb_atom_t netCloseWindow_ = XCB_ATOM_NONE;
    xcb_atom_t netNumberOfDesktops_ = XCB_ATOM_NONE;
    xcb_atom_t netCurrentDesktop_ = XCB_ATOM_NONE;
    xcb_atom_t netWmDesktop_ = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowType_ = XCB_ATOM_NONE;
    xcb_atom_t netWmWindowTypeDock_ = XCB_ATOM_NONE;
    xcb_atom_t netWmStrutPartial_ = XCB_ATOM_NONE;
    inline static xcb_connection_t *serverAnchor_ = nullptr;
};

TEST_F(EwmhCapabilitiesIntegrationTest, MissingOwnerIsARecoverableUnavailableCapability) {
    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::unavailable);
    EXPECT_EQ(ewmhDiscoveryStatusId(result.value().status), "ewmh_unavailable");
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, VerifiesOwnerAndReportsOnlyAdvertisedPd1Features) {
    const std::array supported{netClientList_,       netActiveWindow_,     netCloseWindow_,
                               netNumberOfDesktops_, netCurrentDesktop_,   netWmDesktop_,
                               netWmWindowType_,     netWmWindowTypeDock_, netWmStrutPartial_};
    configureVerifiedOwner(supported);

    const auto result = discover();

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().status, EwmhDiscoveryStatus::verified);
    EXPECT_EQ(ewmhDiscoveryStatusId(result.value().status), "ewmh_verified");
    EXPECT_EQ(result.value().flags, (EwmhCapabilityFlags{true, true, true, true, true, true}));
}

TEST_F(EwmhCapabilitiesIntegrationTest, KeepsIncompleteAdvertisementPreciselyReduced) {
    const std::array supported{netClientList_, netNumberOfDesktops_, netCurrentDesktop_,
                               netWmWindowType_, netWmWindowTypeDock_};
    configureVerifiedOwner(supported);

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::verified);
    EXPECT_EQ(result.value().flags, (EwmhCapabilityFlags{true, false, false, true, false}));
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsOwnerWithoutSupportingWindowSelfReference) {
    createSupportingWindow();
    setWindowProperty(root_, supportingCheck_, supportingWindow_);
    setWindowProperty(supportingWindow_, supportingCheck_, root_);
    const std::array supported{netClientList_};
    setSupportedAtoms(supported);
    flushChecked();

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(ewmhDiscoveryStatusId(result.value().status), "ewmh_malformed");
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsRootOwnerWithWrongProtocolType) {
    createSupportingWindow();
    const std::array value{static_cast<xcb_atom_t>(supportingWindow_)};
    const auto cookie =
        xcb_change_property_checked(connection_, XCB_PROP_MODE_REPLACE, root_, supportingCheck_,
                                    XCB_ATOM_ATOM, 32, value.size(), value.data());
    ProtocolError error{xcb_request_check(connection_, cookie)};
    ASSERT_FALSE(error);
    flushChecked();

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsVerifiedOwnerWithoutSupportedSet) {
    createSupportingWindow();
    setWindowProperty(root_, supportingCheck_, supportingWindow_);
    setWindowProperty(supportingWindow_, supportingCheck_, supportingWindow_);
    flushChecked();

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsMalformedSupportedPropertyWithoutFailingTransport) {
    createSupportingWindow();
    setWindowProperty(root_, supportingCheck_, supportingWindow_);
    setWindowProperty(supportingWindow_, supportingCheck_, supportingWindow_);
    const std::array supported{netClientList_};
    setSupportedAtoms(supported, XCB_ATOM_CARDINAL);
    flushChecked();

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsNoneInSupportedAtomSet) {
    const std::array supported{static_cast<xcb_atom_t>(XCB_ATOM_NONE), netClientList_};
    configureVerifiedOwner(supported);

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, ReplacementDuringDiscoveryPublishesNoPartialFlags) {
    const std::array supported{netClientList_};
    configureVerifiedOwner(supported);
    replacementWindow_ = createWindow();
    ASSERT_NE(replacementWindow_, XCB_WINDOW_NONE);
    setWindowProperty(replacementWindow_, supportingCheck_, replacementWindow_);
    flushChecked();

    const auto result = discover(&replaceOwnerDuringDiscovery, this);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, DisappearanceDuringDiscoveryPublishesNoPartialFlags) {
    const std::array supported{netClientList_};
    configureVerifiedOwner(supported);

    const auto result = discover(&removeOwnerDuringDiscovery, this);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

TEST_F(EwmhCapabilitiesIntegrationTest, RejectsOversizedSupportedSetWithoutPartialClaims) {
    std::vector<xcb_atom_t> supported(maximumEwmhSupportedAtoms + 1U, netClientList_);
    configureVerifiedOwner(supported);

    const auto result = discover();

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status, EwmhDiscoveryStatus::malformed);
    EXPECT_EQ(result.value().flags, EwmhCapabilityFlags{});
}

} // namespace
} // namespace prismdrake::x11
