#include "EwmhTaskSource.hpp"
#include "RootEventStream.hpp"
#include "TaskModel.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using namespace std::chrono_literals;

struct FreeDeleter final {
    void operator()(void *pointer) const noexcept { std::free(pointer); }
};

using InternReply = std::unique_ptr<xcb_intern_atom_reply_t, FreeDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

struct TestAtoms final {
    xcb_atom_t supportingCheck;
    xcb_atom_t netSupported;
    xcb_atom_t clientList;
    xcb_atom_t clientListStacking;
    xcb_atom_t activeWindow;
    xcb_atom_t utf8String;
    xcb_atom_t netWmName;
    xcb_atom_t wmClass;
    xcb_atom_t wmHints;
    xcb_atom_t wmTransientFor;
    xcb_atom_t netWmDesktop;
    xcb_atom_t netWmIcon;
    xcb_atom_t netWmWindowType;
    xcb_atom_t netWmWindowTypeNormal;
    xcb_atom_t netWmState;
    xcb_atom_t netWmStateHidden;
    xcb_atom_t netWmStateSkipTaskbar;
    xcb_atom_t netWmStateDemandsAttention;
};

class EwmhTaskSourceIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        display_ = std::getenv("DISPLAY");
        ASSERT_NE(display_, nullptr);
        ASSERT_FALSE(std::string_view{display_}.empty());

        int screenIndex = 0;
        control_ = xcb_connect(display_, &screenIndex);
        ASSERT_NE(control_, nullptr);
        ASSERT_EQ(xcb_connection_has_error(control_), 0);

        const auto *setup = xcb_get_setup(control_);
        ASSERT_NE(setup, nullptr);
        auto iterator = xcb_setup_roots_iterator(setup);
        for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
            xcb_screen_next(&iterator);
        }
        ASSERT_GT(iterator.rem, 0);
        ASSERT_NE(iterator.data, nullptr);
        root_ = iterator.data->root;

        atoms_ = TestAtoms{intern("_NET_SUPPORTING_WM_CHECK"),
                           intern("_NET_SUPPORTED"),
                           intern("_NET_CLIENT_LIST"),
                           intern("_NET_CLIENT_LIST_STACKING"),
                           intern("_NET_ACTIVE_WINDOW"),
                           intern("UTF8_STRING"),
                           intern("_NET_WM_NAME"),
                           intern("WM_CLASS"),
                           intern("WM_HINTS"),
                           intern("WM_TRANSIENT_FOR"),
                           intern("_NET_WM_DESKTOP"),
                           intern("_NET_WM_ICON"),
                           intern("_NET_WM_WINDOW_TYPE"),
                           intern("_NET_WM_WINDOW_TYPE_NORMAL"),
                           intern("_NET_WM_STATE"),
                           intern("_NET_WM_STATE_HIDDEN"),
                           intern("_NET_WM_STATE_SKIP_TASKBAR"),
                           intern("_NET_WM_STATE_DEMANDS_ATTENTION")};
        ASSERT_NE(atoms_.supportingCheck, XCB_ATOM_NONE);
        clearRootContract();
        flushChecked();
    }

    void TearDown() override {
        if (control_ == nullptr) {
            return;
        }
        clearRootContract();
        for (const auto window : resources_) {
            if (window != XCB_WINDOW_NONE) {
                (void)xcb_destroy_window(control_, window);
            }
        }
        (void)xcb_flush(control_);
        xcb_disconnect(control_);
        control_ = nullptr;
    }

    [[nodiscard]] xcb_atom_t intern(std::string_view name) {
        xcb_generic_error_t *rawError = nullptr;
        InternReply reply{xcb_intern_atom_reply(
            control_,
            xcb_intern_atom(control_, 0, static_cast<std::uint16_t>(name.size()), name.data()),
            &rawError)};
        ProtocolError error{rawError};
        if (error || !reply) {
            ADD_FAILURE() << "failed to intern a fixed test atom";
            return XCB_ATOM_NONE;
        }
        return reply->atom;
    }

    [[nodiscard]] xcb_window_t createWindow() {
        const auto window = xcb_generate_id(control_);
        createWindowWithId(window);
        resources_.push_back(window);
        return window;
    }

    void createWindowWithId(xcb_window_t window) {
        const auto cookie = xcb_create_window_checked(
            control_, XCB_COPY_FROM_PARENT, window, root_, 20, 20, 320U, 180U, 0U,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0U, nullptr);
        ProtocolError error{xcb_request_check(control_, cookie)};
        ASSERT_FALSE(error);
        ASSERT_NE(window, XCB_WINDOW_NONE);
    }

    void destroyWindow(xcb_window_t window) {
        const auto cookie = xcb_destroy_window_checked(control_, window);
        ProtocolError error{xcb_request_check(control_, cookie)};
        ASSERT_FALSE(error);
        flushChecked();
    }

    void clearRootContract() {
        for (const auto property : {atoms_.supportingCheck, atoms_.netSupported, atoms_.clientList,
                                    atoms_.clientListStacking, atoms_.activeWindow}) {
            if (property != XCB_ATOM_NONE) {
                (void)xcb_delete_property(control_, root_, property);
            }
        }
    }

    void flushChecked() {
        ASSERT_GE(xcb_flush(control_), 0);
        ASSERT_EQ(xcb_connection_has_error(control_), 0);
    }

    void replace32(xcb_window_t target, xcb_atom_t property, xcb_atom_t type,
                   std::span<const std::uint32_t> values) {
        const auto cookie = xcb_change_property_checked(
            control_, XCB_PROP_MODE_REPLACE, target, property, type, 32,
            static_cast<std::uint32_t>(values.size()), values.empty() ? nullptr : values.data());
        ProtocolError error{xcb_request_check(control_, cookie)};
        ASSERT_FALSE(error);
    }

    void replace8(xcb_window_t target, xcb_atom_t property, xcb_atom_t type,
                  std::span<const std::uint8_t> values) {
        const auto cookie = xcb_change_property_checked(
            control_, XCB_PROP_MODE_REPLACE, target, property, type, 8,
            static_cast<std::uint32_t>(values.size()), values.empty() ? nullptr : values.data());
        ProtocolError error{xcb_request_check(control_, cookie)};
        ASSERT_FALSE(error);
    }

    void configureOwner(xcb_window_t owner) {
        const std::array<std::uint32_t, 1U> ownerValue{owner};
        replace32(root_, atoms_.supportingCheck, XCB_ATOM_WINDOW, ownerValue);
        replace32(owner, atoms_.supportingCheck, XCB_ATOM_WINDOW, ownerValue);
        const std::array<std::uint32_t, 2U> supported{atoms_.clientList, atoms_.activeWindow};
        replace32(root_, atoms_.netSupported, XCB_ATOM_ATOM, supported);
    }

    void configureRoot(std::span<const std::uint32_t> clients,
                       std::span<const std::uint32_t> stacking,
                       std::optional<std::uint32_t> active) {
        replace32(root_, atoms_.clientList, XCB_ATOM_WINDOW, clients);
        replace32(root_, atoms_.clientListStacking, XCB_ATOM_WINDOW, stacking);
        if (active) {
            const std::array<std::uint32_t, 1U> value{active.value()};
            replace32(root_, atoms_.activeWindow, XCB_ATOM_WINDOW, value);
        } else {
            (void)xcb_delete_property(control_, root_, atoms_.activeWindow);
        }
    }

    void configureClient(xcb_window_t window, std::string_view title,
                         std::span<const std::uint32_t> states = {}) {
        const auto *titleData = reinterpret_cast<const std::uint8_t *>(title.data());
        replace8(window, atoms_.netWmName, atoms_.utf8String, {titleData, title.size()});
        constexpr std::array<std::uint8_t, 21U> wmClass{'d', 'e', 'm', 'o', 0U,  'P', 'r',
                                                        'i', 's', 'm', 'd', 'r', 'a', 'k',
                                                        'e', 'T', 'e', 's', 't', 0U,  0U};
        // WM_CLASS contains exactly two NUL-terminated strings and no trailing payload.
        replace8(window, atoms_.wmClass, XCB_ATOM_STRING,
                 std::span<const std::uint8_t>{wmClass}.first(wmClass.size() - 1U));
        const std::array<std::uint32_t, 1U> type{atoms_.netWmWindowTypeNormal};
        replace32(window, atoms_.netWmWindowType, XCB_ATOM_ATOM, type);
        replace32(window, atoms_.netWmState, XCB_ATOM_ATOM, states);
        const std::array<std::uint32_t, 1U> workspace{3U};
        replace32(window, atoms_.netWmDesktop, XCB_ATOM_CARDINAL, workspace);
        const std::array<std::uint32_t, 9U> hints{};
        replace32(window, atoms_.wmHints, atoms_.wmHints, hints);
        constexpr std::array<std::uint32_t, 6U> icon{2U,          2U,          0xff102030U,
                                                     0xff405060U, 0xff708090U, 0xffa0b0c0U};
        replace32(window, atoms_.netWmIcon, XCB_ATOM_CARDINAL, icon);
    }

    [[nodiscard]] static std::optional<ClientPropertyHint>
    waitForPropertyHint(RootEventStream &stream, WindowId::Value expectedWindow,
                        xcb_atom_t expectedAtom, int eventFileDescriptor) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd descriptor{eventFileDescriptor, POLLIN, 0};
            int pollResult = -1;
            do {
                pollResult = ::poll(&descriptor, 1U, static_cast<int>(remaining.count()));
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                return std::nullopt;
            }
            auto batch = stream.drain();
            if (!batch) {
                return std::nullopt;
            }
            for (const auto &event : batch.value().events) {
                if (const auto *hint = std::get_if<ClientPropertyHint>(&event);
                    hint != nullptr && hint->window.value() == expectedWindow &&
                    hint->atom.value() == expectedAtom) {
                    return *hint;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<ClientTopologyHint>
    waitForTopologyHint(RootEventStream &stream, WindowId::Value expectedWindow,
                        ClientTopologyChange expectedChange, int eventFileDescriptor) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd descriptor{eventFileDescriptor, POLLIN, 0};
            int pollResult = -1;
            do {
                pollResult = ::poll(&descriptor, 1U, static_cast<int>(remaining.count()));
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                return std::nullopt;
            }
            auto batch = stream.drain();
            if (!batch) {
                return std::nullopt;
            }
            for (const auto &event : batch.value().events) {
                if (const auto *hint = std::get_if<ClientTopologyHint>(&event);
                    hint != nullptr && hint->window.value() == expectedWindow &&
                    hint->change == expectedChange) {
                    return *hint;
                }
            }
        }
        return std::nullopt;
    }

    const char *display_ = nullptr;
    xcb_connection_t *control_ = nullptr;
    xcb_window_t root_ = XCB_WINDOW_NONE;
    TestAtoms atoms_{};
    std::vector<xcb_window_t> resources_;
};

TEST_F(EwmhTaskSourceIntegrationTest, PublishesBoundedMetadataFromVerifiedOwner) {
    const auto owner = createWindow();
    const auto first = createWindow();
    const auto second = createWindow();
    configureOwner(owner);
    const std::array<std::uint32_t, 2U> clients{first, second};
    const std::array<std::uint32_t, 2U> stacking{second, first};
    configureRoot(clients, stacking, first);
    const std::array<std::uint32_t, 1U> firstStates{atoms_.netWmStateHidden};
    configureClient(first, "First task", firstStates);
    const std::array<std::uint32_t, 1U> secondStates{atoms_.netWmStateDemandsAttention};
    configureClient(second, "Second task", secondStates);
    flushChecked();

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto source = EwmhTaskSource::create(connection.value());
    ASSERT_TRUE(source);
    auto observation = source.value().refresh(connection.value());
    ASSERT_TRUE(observation);
    ASSERT_EQ(observation.value().authoritative.clientList().size(), 2U);
    ASSERT_EQ(observation.value().authoritative.stackingOrder().size(), 2U);
    EXPECT_EQ(observation.value().authoritative.stackingOrder()[0].value(), second);
    ASSERT_TRUE(observation.value().authoritative.activeWindow());
    EXPECT_EQ(observation.value().authoritative.activeWindow()->value(), first);

    TaskModel model;
    auto snapshot = model.publish(observation.value());
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot.value()->tasks().size(), 2U);
    EXPECT_EQ(snapshot.value()->tasks()[0].window().value(), first);
    EXPECT_EQ(snapshot.value()->tasks()[0].title(), "First task");
    EXPECT_EQ(snapshot.value()->tasks()[0].applicationId(), "PrismdrakeTest");
    EXPECT_EQ(snapshot.value()->tasks()[0].workspace(), 3U);
    EXPECT_EQ(snapshot.value()->tasks()[0].type(), WindowType::normal);
    EXPECT_TRUE(snapshot.value()->tasks()[0].active());
    EXPECT_TRUE(snapshot.value()->tasks()[0].hidden());
    EXPECT_FALSE(snapshot.value()->tasks()[0].urgent());
    EXPECT_EQ(snapshot.value()->tasks()[0].fallbackIconName(), "application-x-executable");
    EXPECT_FALSE(snapshot.value()->tasks()[1].active());
    EXPECT_TRUE(snapshot.value()->tasks()[1].urgent());
}

TEST_F(EwmhTaskSourceIntegrationTest, ClientPropertyHintsRefreshOneStableLifetime) {
    const auto owner = createWindow();
    const auto client = createWindow();
    configureOwner(owner);
    const std::array<std::uint32_t, 1U> clients{client};
    configureRoot(clients, clients, client);
    configureClient(client, "Before refresh");
    flushChecked();

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto stream = RootEventStream::create(connection.value());
    ASSERT_TRUE(stream);
    auto source = EwmhTaskSource::create(connection.value());
    ASSERT_TRUE(source);
    TaskModel model;
    auto firstObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(firstObservation);
    auto firstSnapshot = model.publish(firstObservation.value());
    ASSERT_TRUE(firstSnapshot);
    ASSERT_EQ(firstSnapshot.value()->tasks().size(), 1U);
    const auto lifetime = firstSnapshot.value()->tasks()[0].lifetime();

    auto drained = stream.value().drain();
    ASSERT_TRUE(drained);
    constexpr std::string_view changedTitle{"After refresh"};
    replace8(client, atoms_.netWmName, atoms_.utf8String,
             {reinterpret_cast<const std::uint8_t *>(changedTitle.data()), changedTitle.size()});
    flushChecked();
    const auto titleHint = waitForPropertyHint(stream.value(), client, atoms_.netWmName,
                                               connection.value().eventFileDescriptor());
    ASSERT_TRUE(titleHint);
    EXPECT_FALSE(titleHint->synthetic);

    const std::array<std::uint32_t, 2U> changedStates{atoms_.netWmStateDemandsAttention,
                                                      atoms_.netWmStateSkipTaskbar};
    replace32(client, atoms_.netWmState, XCB_ATOM_ATOM, changedStates);
    flushChecked();
    const auto stateHint = waitForPropertyHint(stream.value(), client, atoms_.netWmState,
                                               connection.value().eventFileDescriptor());
    ASSERT_TRUE(stateHint);

    auto excludedObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(excludedObservation);
    auto excludedSnapshot = model.publish(excludedObservation.value());
    ASSERT_TRUE(excludedSnapshot);
    EXPECT_TRUE(excludedSnapshot.value()->tasks().empty());

    const std::array<std::uint32_t, 1U> urgentOnly{atoms_.netWmStateDemandsAttention};
    replace32(client, atoms_.netWmState, XCB_ATOM_ATOM, urgentOnly);
    flushChecked();
    auto includedObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(includedObservation);
    auto includedSnapshot = model.publish(includedObservation.value());
    ASSERT_TRUE(includedSnapshot);
    ASSERT_EQ(includedSnapshot.value()->tasks().size(), 1U);
    EXPECT_EQ(includedSnapshot.value()->tasks()[0].lifetime(), lifetime);
    EXPECT_EQ(includedSnapshot.value()->tasks()[0].title(), "After refresh");
    EXPECT_TRUE(includedSnapshot.value()->tasks()[0].urgent());
}

TEST_F(EwmhTaskSourceIntegrationTest,
       ExcludesDestroyedAdvertisedClientAndChangesLifetimeAfterReuse) {
    const auto owner = createWindow();
    const auto client = createWindow();
    configureOwner(owner);
    const std::array<std::uint32_t, 1U> clients{client};
    configureRoot(clients, clients, client);
    configureClient(client, "First incarnation");
    flushChecked();

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto stream = RootEventStream::create(connection.value());
    ASSERT_TRUE(stream);
    auto source = EwmhTaskSource::create(connection.value());
    ASSERT_TRUE(source);
    TaskModel model;
    auto firstObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(firstObservation);
    auto firstSnapshot = model.publish(firstObservation.value());
    ASSERT_TRUE(firstSnapshot);
    ASSERT_EQ(firstSnapshot.value()->tasks().size(), 1U);
    const auto firstLifetime = firstSnapshot.value()->tasks()[0].lifetime();

    auto drained = stream.value().drain();
    ASSERT_TRUE(drained);
    destroyWindow(client);
    const auto destroyed =
        waitForTopologyHint(stream.value(), client, ClientTopologyChange::destroyed,
                            connection.value().eventFileDescriptor());
    ASSERT_TRUE(destroyed);
    EXPECT_FALSE(destroyed->synthetic);
    auto staleObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(staleObservation);
    auto staleSnapshot = model.publish(staleObservation.value());
    ASSERT_TRUE(staleSnapshot);
    EXPECT_TRUE(staleSnapshot.value()->tasks().empty());

    source.value().invalidateClient(destroyed->window);
    createWindowWithId(client);
    configureClient(client, "Second incarnation");
    flushChecked();
    auto reusedObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(reusedObservation);
    auto reusedSnapshot = model.publish(reusedObservation.value());
    ASSERT_TRUE(reusedSnapshot);
    ASSERT_EQ(reusedSnapshot.value()->tasks().size(), 1U);
    EXPECT_EQ(reusedSnapshot.value()->tasks()[0].window().value(), client);
    EXPECT_EQ(reusedSnapshot.value()->tasks()[0].title(), "Second incarnation");
    EXPECT_NE(reusedSnapshot.value()->tasks()[0].lifetime(), firstLifetime);
}

TEST_F(EwmhTaskSourceIntegrationTest, CanonicalizesNoneAndResetsIncarnationForNewOwner) {
    const auto firstOwner = createWindow();
    const auto replacementOwner = createWindow();
    const auto client = createWindow();
    configureOwner(firstOwner);
    const std::array<std::uint32_t, 1U> clients{client};
    configureRoot(clients, clients, XCB_WINDOW_NONE);
    configureClient(client, "Owner transition");
    flushChecked();

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto source = EwmhTaskSource::create(connection.value());
    ASSERT_TRUE(source);
    TaskModel model;
    auto firstObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(firstObservation);
    EXPECT_FALSE(firstObservation.value().authoritative.activeWindow());
    auto firstSnapshot = model.publish(firstObservation.value());
    ASSERT_TRUE(firstSnapshot);
    ASSERT_EQ(firstSnapshot.value()->tasks().size(), 1U);
    EXPECT_FALSE(firstSnapshot.value()->tasks()[0].active());
    const auto firstLifetime = firstSnapshot.value()->tasks()[0].lifetime();

    configureOwner(replacementOwner);
    flushChecked();
    auto replacementObservation = source.value().refresh(connection.value());
    ASSERT_TRUE(replacementObservation);
    auto replacementSnapshot = model.publish(replacementObservation.value());
    ASSERT_TRUE(replacementSnapshot);
    ASSERT_EQ(replacementSnapshot.value()->tasks().size(), 1U);
    EXPECT_NE(replacementSnapshot.value()->tasks()[0].lifetime(), firstLifetime);
}

TEST_F(EwmhTaskSourceIntegrationTest, ExcludesMalformedAndOversizedIconMetadata) {
    const auto owner = createWindow();
    const auto malformed = createWindow();
    const auto oversized = createWindow();
    configureOwner(owner);
    const std::array<std::uint32_t, 2U> clients{malformed, oversized};
    configureRoot(clients, clients, std::nullopt);
    configureClient(malformed, "Malformed icon");
    configureClient(oversized, "Oversized icon");
    const std::array<std::uint32_t, 3U> truncatedIcon{2U, 2U, 0xff000000U};
    replace32(malformed, atoms_.netWmIcon, XCB_ATOM_CARDINAL, truncatedIcon);
    const std::array<std::uint32_t, 3U> oversizedIcon{maximumWindowIconDimension + 1U, 1U,
                                                      0xff000000U};
    replace32(oversized, atoms_.netWmIcon, XCB_ATOM_CARDINAL, oversizedIcon);
    flushChecked();

    auto connection = X11Connection::connect(display_);
    ASSERT_TRUE(connection);
    auto source = EwmhTaskSource::create(connection.value());
    ASSERT_TRUE(source);
    auto observation = source.value().refresh(connection.value());
    ASSERT_TRUE(observation);
    ASSERT_EQ(observation.value().windows.size(), 2U);
    EXPECT_FALSE(observation.value().windows[0].metadata);
    EXPECT_FALSE(observation.value().windows[1].metadata);

    TaskModel model;
    auto snapshot = model.publish(observation.value());
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(snapshot.value()->tasks().empty());
}

} // namespace
} // namespace prismdrake::x11
