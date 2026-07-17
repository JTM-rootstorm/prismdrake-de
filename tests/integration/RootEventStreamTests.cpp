#include "RootEventStream.hpp"

#include "RandrTopology.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <variant>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using namespace std::chrono_literals;

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

using ControlConnection = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

class ControlledWindow final {
  public:
    ControlledWindow(xcb_connection_t &connection, xcb_window_t root)
        : connection_(&connection), window_(xcb_generate_id(&connection)) {
        const auto cookie = xcb_create_window_checked(
            &connection, XCB_COPY_FROM_PARENT, window_, root, 10, 10, 320U, 200U, 0U,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0U, nullptr);
        ProtocolError error{xcb_request_check(&connection, cookie)};
        if (error || window_ == XCB_WINDOW_NONE) {
            throw std::runtime_error("controlled X11 window creation failed");
        }
    }

    ~ControlledWindow() {
        if (connection_ != nullptr && window_ != XCB_WINDOW_NONE) {
            (void)xcb_destroy_window(connection_, window_);
            (void)xcb_flush(connection_);
        }
    }

    ControlledWindow(const ControlledWindow &) = delete;
    ControlledWindow &operator=(const ControlledWindow &) = delete;

    [[nodiscard]] xcb_window_t id() const noexcept { return window_; }

    void destroy() {
        const auto cookie = xcb_destroy_window_checked(connection_, window_);
        ProtocolError error{xcb_request_check(connection_, cookie)};
        if (error) {
            throw std::runtime_error("controlled X11 window destruction failed");
        }
        window_ = XCB_WINDOW_NONE;
    }

  private:
    xcb_connection_t *connection_;
    xcb_window_t window_;
};

[[nodiscard]] xcb_screen_t *selectedScreen(xcb_connection_t &connection, int screenIndex) {
    const auto *setup = xcb_get_setup(&connection);
    if (setup == nullptr) {
        return nullptr;
    }
    auto iterator = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenIndex && iterator.rem > 0; ++index) {
        xcb_screen_next(&iterator);
    }
    return iterator.rem > 0 ? iterator.data : nullptr;
}

[[nodiscard]] std::optional<ClientTopologyHint>
waitForTopologyHint(RootEventStream &stream, ClientTopologyChange expectedChange,
                    WindowId::Value expectedWindow, int eventFileDescriptor) {
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
                hint != nullptr && hint->change == expectedChange &&
                hint->window.value() == expectedWindow) {
                return *hint;
            }
        }
    }
    return std::nullopt;
}

TEST(RootEventStreamIntegrationTest, ObservesControlledClientCreateAndDestroyAsHints) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ASSERT_FALSE(std::string_view{display}.empty());

    auto observer = X11Connection::connect(display);
    ASSERT_TRUE(observer);
    auto stream = RootEventStream::create(observer.value());
    ASSERT_TRUE(stream);
    EXPECT_FALSE(stream.value().drain(0U));
    EXPECT_FALSE(stream.value().drain(maximumRootEventsPerDrain + 1U));

    int controlScreenIndex = 0;
    ControlConnection control{xcb_connect(display, &controlScreenIndex)};
    ASSERT_TRUE(control);
    ASSERT_EQ(xcb_connection_has_error(control.get()), 0);
    auto *screen = selectedScreen(*control, controlScreenIndex);
    ASSERT_NE(screen, nullptr);
    ASSERT_EQ(screen->root, observer.value().screen().rootWindow.value());

    ControlledWindow window{*control, screen->root};
    const auto created = waitForTopologyHint(stream.value(), ClientTopologyChange::created,
                                             window.id(), observer.value().eventFileDescriptor());
    ASSERT_TRUE(created);
    EXPECT_EQ(created->window.value(), window.id());
    EXPECT_FALSE(created->synthetic);

    const auto destroyedWindow = window.id();
    window.destroy();
    const auto destroyed =
        waitForTopologyHint(stream.value(), ClientTopologyChange::destroyed, destroyedWindow,
                            observer.value().eventFileDescriptor());
    ASSERT_TRUE(destroyed);
    EXPECT_EQ(destroyed->window.value(), destroyedWindow);
    EXPECT_FALSE(destroyed->synthetic);
}

TEST(RootEventStreamIntegrationTest, RandrAwareSoleDrainPreservesCoreLifecycleEvents) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ASSERT_FALSE(std::string_view{display}.empty());

    auto observer = X11Connection::connect(display);
    ASSERT_TRUE(observer);
    auto protocol = RandrTopologyProtocol::negotiate(observer.value());
    ASSERT_TRUE(protocol);
    if (protocol.value().status() == RandrTopologyStatus::unavailable) {
        GTEST_SKIP() << "isolated X server has no RandR 1.2 support";
    }
    ASSERT_NE(protocol.value().status(), RandrTopologyStatus::malformed);
    auto stream = RootEventStream::create(observer.value(), protocol.value());
    ASSERT_TRUE(stream);

    int controlScreenIndex = 0;
    ControlConnection control{xcb_connect(display, &controlScreenIndex)};
    ASSERT_TRUE(control);
    ASSERT_EQ(xcb_connection_has_error(control.get()), 0);
    auto *screen = selectedScreen(*control, controlScreenIndex);
    ASSERT_NE(screen, nullptr);

    ControlledWindow window{*control, screen->root};
    const auto created = waitForTopologyHint(stream.value(), ClientTopologyChange::created,
                                             window.id(), observer.value().eventFileDescriptor());
    ASSERT_TRUE(created);
    EXPECT_EQ(created->window.value(), window.id());
    EXPECT_FALSE(created->synthetic);

    const auto destroyedWindow = window.id();
    window.destroy();
    const auto destroyed =
        waitForTopologyHint(stream.value(), ClientTopologyChange::destroyed, destroyedWindow,
                            observer.value().eventFileDescriptor());
    ASSERT_TRUE(destroyed);
    EXPECT_EQ(destroyed->window.value(), destroyedWindow);
    EXPECT_FALSE(destroyed->synthetic);
}

TEST(RootEventStreamIntegrationTest, RejectsDuplicateThenReleasesSubscriptionForRecreation) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);

    auto observer = X11Connection::connect(display);
    ASSERT_TRUE(observer);
    {
        auto first = RootEventStream::create(observer.value());
        ASSERT_TRUE(first);
        EXPECT_FALSE(RootEventStream::create(observer.value()));
    }

    auto recreated = RootEventStream::create(observer.value());
    ASSERT_TRUE(recreated);
}

TEST(RootEventStreamIntegrationTest, ConnectionDestructionDoesNotInvalidateActiveStream) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);

    std::optional<RootEventStream> retainedStream;
    int observerFileDescriptor = -1;
    WindowId::Value root = 0U;
    {
        auto observer = X11Connection::connect(display);
        ASSERT_TRUE(observer);
        observerFileDescriptor = observer.value().eventFileDescriptor();
        root = observer.value().screen().rootWindow.value();
        auto stream = RootEventStream::create(observer.value());
        ASSERT_TRUE(stream);
        retainedStream.emplace(std::move(stream).value());
    }

    int controlScreenIndex = 0;
    ControlConnection control{xcb_connect(display, &controlScreenIndex)};
    ASSERT_TRUE(control);
    ASSERT_EQ(xcb_connection_has_error(control.get()), 0);
    auto *screen = selectedScreen(*control, controlScreenIndex);
    ASSERT_NE(screen, nullptr);
    ASSERT_EQ(screen->root, root);

    ControlledWindow window{*control, screen->root};
    const auto created = waitForTopologyHint(*retainedStream, ClientTopologyChange::created,
                                             window.id(), observerFileDescriptor);
    ASSERT_TRUE(created);
    EXPECT_EQ(created->window.value(), window.id());
    const auto destroyedWindow = window.id();
    window.destroy();
    const auto destroyed = waitForTopologyHint(*retainedStream, ClientTopologyChange::destroyed,
                                               destroyedWindow, observerFileDescriptor);
    ASSERT_TRUE(destroyed);
    EXPECT_EQ(destroyed->window.value(), destroyedWindow);
}

TEST(RootEventStreamIntegrationTest, ReleaseDoesNotQueueTopologyHintsWhileInactive) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);

    auto observer = X11Connection::connect(display);
    ASSERT_TRUE(observer);
    {
        auto stream = RootEventStream::create(observer.value());
        ASSERT_TRUE(stream);
    }

    int controlScreenIndex = 0;
    ControlConnection control{xcb_connect(display, &controlScreenIndex)};
    ASSERT_TRUE(control);
    ASSERT_EQ(xcb_connection_has_error(control.get()), 0);
    auto *screen = selectedScreen(*control, controlScreenIndex);
    ASSERT_NE(screen, nullptr);

    ControlledWindow gapWindow{*control, screen->root};
    gapWindow.destroy();

    auto recreated = RootEventStream::create(observer.value());
    ASSERT_TRUE(recreated);
    auto pending = recreated.value().drain();
    ASSERT_TRUE(pending);
    for (const auto &event : pending.value().events) {
        EXPECT_EQ(std::get_if<ClientTopologyHint>(&event), nullptr);
    }
}

} // namespace
} // namespace prismdrake::x11
