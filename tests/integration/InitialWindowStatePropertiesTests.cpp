#include "InitialWindowStateProperties.hpp"

#include "AtomCache.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

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

class InitialWindowStatePropertiesIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        int screenIndex = 0;
        observer_.reset(xcb_connect(display_, &screenIndex));
        ASSERT_TRUE(observer_);
        ASSERT_EQ(xcb_connection_has_error(observer_.get()), 0);
        auto iterator = xcb_setup_roots_iterator(xcb_get_setup(observer_.get()));
        for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
            xcb_screen_next(&iterator);
        }
        ASSERT_GT(iterator.rem, 0);
        screen_ = iterator.data;
        atom_ = intern("ATOM");
        cardinal_ = intern("CARDINAL");
        state_ = intern("_NET_WM_STATE");
        skipTaskbar_ = intern("_NET_WM_STATE_SKIP_TASKBAR");
        modal_ = intern("_NET_WM_STATE_MODAL");
        demandsAttention_ = intern("_NET_WM_STATE_DEMANDS_ATTENTION");
    }

    void TearDown() override {
        if (observer_ && window_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(observer_.get(), window_);
            (void)xcb_flush(observer_.get());
        }
    }

    [[nodiscard]] xcb_atom_t intern(std::string_view name) {
        xcb_generic_error_t *rawError = nullptr;
        InternReply reply{xcb_intern_atom_reply(
            observer_.get(),
            xcb_intern_atom(observer_.get(), 0U, static_cast<std::uint16_t>(name.size()),
                            name.data()),
            &rawError)};
        ProtocolError error{rawError};
        EXPECT_FALSE(error);
        EXPECT_TRUE(reply);
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }

    [[nodiscard]] WindowId createWindow(bool overrideRedirect = false) {
        window_ = xcb_generate_id(observer_.get());
        const std::uint32_t values[]{overrideRedirect ? 1U : 0U};
        ProtocolError error{xcb_request_check(
            observer_.get(),
            xcb_create_window_checked(observer_.get(), screen_->root_depth, window_, screen_->root,
                                      0, 0, 100U, 60U, 0U, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                      screen_->root_visual, XCB_CW_OVERRIDE_REDIRECT, values))};
        EXPECT_FALSE(error);
        auto converted = WindowId::fromProtocol(window_);
        EXPECT_TRUE(converted);
        return converted.value();
    }

    void replaceState(xcb_atom_t type, std::span<const std::uint32_t> values) {
        ProtocolError error{xcb_request_check(
            observer_.get(), xcb_change_property_checked(
                                 observer_.get(), XCB_PROP_MODE_REPLACE, window_, state_, type, 32U,
                                 static_cast<std::uint32_t>(values.size()), values.data()))};
        ASSERT_FALSE(error);
    }

    [[nodiscard]] std::vector<std::uint32_t> stateValues() const {
        xcb_generic_error_t *rawError = nullptr;
        PropertyReply reply{xcb_get_property_reply(
            observer_.get(), xcb_get_property(observer_.get(), 0U, window_, state_, atom_, 0U, 64U),
            &rawError)};
        ProtocolError error{rawError};
        EXPECT_FALSE(error);
        EXPECT_TRUE(reply);
        if (!reply || reply->type != atom_ || reply->format != 32U || reply->bytes_after != 0U) {
            return {};
        }
        const auto bytes = xcb_get_property_value_length(reply.get());
        EXPECT_GE(bytes, 0);
        EXPECT_EQ(bytes % static_cast<int>(sizeof(std::uint32_t)), 0);
        std::vector<std::uint32_t> values(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
        if (!values.empty()) {
            std::memcpy(values.data(), xcb_get_property_value(reply.get()),
                        static_cast<std::size_t>(bytes));
        }
        return values;
    }

    [[nodiscard]] foundation::Result<void> publish(WindowId window) const {
        auto connection = X11Connection::connect(display_);
        if (!connection) {
            return foundation::Result<void>::failure(connection.error());
        }
        auto atoms = AtomCache::create(connection.value());
        if (!atoms) {
            return foundation::Result<void>::failure(atoms.error());
        }
        return InitialWindowStateProperties::publishSkipTaskbar(connection.value(), atoms.value(),
                                                                window);
    }

    const char *display_ = nullptr;
    ConnectionHandle observer_;
    const xcb_screen_t *screen_ = nullptr;
    xcb_window_t window_ = XCB_WINDOW_NONE;
    xcb_atom_t atom_ = XCB_ATOM_NONE;
    xcb_atom_t cardinal_ = XCB_ATOM_NONE;
    xcb_atom_t state_ = XCB_ATOM_NONE;
    xcb_atom_t skipTaskbar_ = XCB_ATOM_NONE;
    xcb_atom_t modal_ = XCB_ATOM_NONE;
    xcb_atom_t demandsAttention_ = XCB_ATOM_NONE;
};

TEST_F(InitialWindowStatePropertiesIntegrationTest,
       PreservesBoundedStateAndPublishesSkipTaskbarIdempotentlyBeforeMapping) {
    const auto window = createWindow();
    const std::array initial{modal_, demandsAttention_};
    replaceState(atom_, initial);

    ASSERT_TRUE(publish(window));
    EXPECT_EQ(stateValues(), (std::vector<std::uint32_t>{modal_, demandsAttention_, skipTaskbar_}));
    ASSERT_TRUE(publish(window));
    EXPECT_EQ(stateValues(), (std::vector<std::uint32_t>{modal_, demandsAttention_, skipTaskbar_}));
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, PublishesSkipTaskbarWhenStateIsAbsent) {
    const auto window = createWindow();

    ASSERT_TRUE(publish(window));
    EXPECT_EQ(stateValues(), (std::vector<std::uint32_t>{skipTaskbar_}));
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, RejectsMappedAndOverrideRedirectTargets) {
    const auto mapped = createWindow();
    ProtocolError mapError{
        xcb_request_check(observer_.get(), xcb_map_window_checked(observer_.get(), window_))};
    ASSERT_FALSE(mapError);
    ASSERT_GT(xcb_flush(observer_.get()), 0);
    const auto mappedResult = publish(mapped);
    ASSERT_FALSE(mappedResult);
    EXPECT_EQ(mappedResult.error().code, foundation::ErrorCode::invalid_argument);

    (void)xcb_destroy_window(observer_.get(), window_);
    window_ = XCB_WINDOW_NONE;
    const auto overrideRedirect = createWindow(true);
    const auto overrideResult = publish(overrideRedirect);
    ASSERT_FALSE(overrideResult);
    EXPECT_EQ(overrideResult.error().code, foundation::ErrorCode::invalid_argument);
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, RejectsRootAndForeignAtomCache) {
    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto atoms = AtomCache::create(connection.value());
    ASSERT_TRUE(atoms);
    const auto rootResult = InitialWindowStateProperties::publishSkipTaskbar(
        connection.value(), atoms.value(), connection.value().screen().rootWindow);
    ASSERT_FALSE(rootResult);
    EXPECT_EQ(rootResult.error().code, foundation::ErrorCode::invalid_argument);

    auto foreignConnection = X11Connection::connect(display_);
    ASSERT_TRUE(foreignConnection);
    auto foreignAtoms = AtomCache::create(foreignConnection.value());
    ASSERT_TRUE(foreignAtoms);
    const auto target = createWindow();
    const auto foreignResult = InitialWindowStateProperties::publishSkipTaskbar(
        connection.value(), foreignAtoms.value(), target);
    ASSERT_FALSE(foreignResult);
    EXPECT_EQ(foreignResult.error().code, foundation::ErrorCode::invalid_argument);
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, RejectsMalformedStateLists) {
    const auto malformedWindow = createWindow();
    const std::array malformed{modal_};
    replaceState(cardinal_, malformed);
    const auto wrongTypeResult = publish(malformedWindow);
    ASSERT_FALSE(wrongTypeResult);
    EXPECT_EQ(wrongTypeResult.error().code, foundation::ErrorCode::validation_error);

    const std::array duplicate{modal_, modal_};
    replaceState(atom_, duplicate);
    const auto duplicateResult = publish(malformedWindow);
    ASSERT_FALSE(duplicateResult);
    EXPECT_EQ(duplicateResult.error().code, foundation::ErrorCode::validation_error);

    const std::array<std::uint32_t, 1U> none{XCB_ATOM_NONE};
    replaceState(atom_, none);
    const auto noneResult = publish(malformedWindow);
    ASSERT_FALSE(noneResult);
    EXPECT_EQ(noneResult.error().code, foundation::ErrorCode::validation_error);
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, RejectsBoundedAndTruncatedOversizedState) {
    const auto window = createWindow();
    std::array<std::uint32_t, 16U> bounded{};
    for (std::size_t index = 0; index < bounded.size(); ++index) {
        bounded[index] = intern("_PRISMDRAKE_TEST_STATE_" + std::to_string(index));
    }
    replaceState(atom_, bounded);
    const auto boundedResult = publish(window);
    ASSERT_FALSE(boundedResult);
    EXPECT_EQ(boundedResult.error().code, foundation::ErrorCode::too_large);

    std::array<std::uint32_t, 17U> truncated{};
    for (std::size_t index = 0; index < truncated.size(); ++index) {
        truncated[index] = intern("_PRISMDRAKE_TEST_TRUNCATED_STATE_" + std::to_string(index));
    }
    replaceState(atom_, truncated);
    const auto truncatedResult = publish(window);
    ASSERT_FALSE(truncatedResult);
    EXPECT_EQ(truncatedResult.error().code, foundation::ErrorCode::validation_error);
}

TEST_F(InitialWindowStatePropertiesIntegrationTest, RejectsDestroyedTarget) {
    const auto window = createWindow();
    ProtocolError destroyError{
        xcb_request_check(observer_.get(), xcb_destroy_window_checked(observer_.get(), window_))};
    ASSERT_FALSE(destroyError);
    ASSERT_GT(xcb_flush(observer_.get()), 0);
    window_ = XCB_WINDOW_NONE;

    const auto result = publish(window);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, foundation::ErrorCode::io_error);
}

} // namespace
} // namespace prismdrake::x11
