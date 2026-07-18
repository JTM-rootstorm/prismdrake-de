#include "TaskController.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::shell::taskcontroller {
namespace {

struct FreeDeleter final {
    void operator()(void *value) const noexcept { std::free(value); }
};

using ProtocolError = std::unique_ptr<xcb_generic_error_t, FreeDeleter>;

void ensureApplication() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argc = 1;
    static char executable[] = "task-controller-x11-test";
    static char *argv[] = {executable, nullptr};
    static QCoreApplication application(argc, argv);
    (void)application;
}

[[nodiscard]] xcb_atom_t intern(xcb_connection_t *connection, std::string_view name) {
    auto *reply = xcb_intern_atom_reply(
        connection,
        xcb_intern_atom(connection, 0, static_cast<std::uint16_t>(name.size()), name.data()),
        nullptr);
    if (reply == nullptr) {
        return XCB_ATOM_NONE;
    }
    const auto atom = reply->atom;
    std::free(reply);
    return atom;
}

[[nodiscard]] bool waitForRows(tasks::TaskPresentationModel &presentation, int expected,
                               int timeoutMilliseconds) {
    if (presentation.rowCount() == expected) {
        return true;
    }
    QEventLoop loop;
    const auto publication = QObject::connect(
        &presentation, &tasks::TaskPresentationModel::publicationApplied, &loop, [&] {
            if (presentation.rowCount() == expected) {
                loop.quit();
            }
        });
    QTimer::singleShot(timeoutMilliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(publication);
    return presentation.rowCount() == expected;
}

void runEventLoopFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

[[nodiscard]] bool waitForPublication(tasks::TaskPresentationModel &presentation,
                                      const std::function<bool()> &complete,
                                      int timeoutMilliseconds) {
    if (complete()) {
        return true;
    }
    QEventLoop loop;
    const auto publication = QObject::connect(
        &presentation, &tasks::TaskPresentationModel::publicationApplied, &loop, [&] {
            if (complete()) {
                loop.quit();
            }
        });
    QTimer::singleShot(timeoutMilliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(publication);
    return complete();
}

class ControlledEwmhOwner final {
  public:
    explicit ControlledEwmhOwner(const char *display) {
        int screenIndex = 0;
        connection_ = xcb_connect(display, &screenIndex);
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            return;
        }
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        for (int index = 0; index < screenIndex && screens.rem > 0; ++index) {
            xcb_screen_next(&screens);
        }
        if (screens.rem > 0 && screens.data != nullptr) {
            root_ = screens.data->root;
            visual_ = screens.data->root_visual;
            depth_ = screens.data->root_depth;
        }
    }

    ~ControlledEwmhOwner() {
        if (connection_ != nullptr) {
            if (root_ != XCB_WINDOW_NONE) {
                (void)xcb_delete_property(connection_, root_, supportingWmCheck());
                (void)xcb_delete_property(connection_, root_, netSupported());
                (void)xcb_delete_property(connection_, root_, netClientList());
            }
            if (client_ != XCB_WINDOW_NONE) {
                (void)xcb_destroy_window(connection_, client_);
            }
            if (owner_ != XCB_WINDOW_NONE) {
                (void)xcb_destroy_window(connection_, owner_);
            }
            (void)xcb_flush(connection_);
            xcb_disconnect(connection_);
        }
    }

    [[nodiscard]] bool healthy() const noexcept {
        return connection_ != nullptr && root_ != XCB_WINDOW_NONE &&
               xcb_connection_has_error(connection_) == 0;
    }

    [[nodiscard]] bool publish(bool includeClient) {
        if (!healthy() || !ensureOwner() || (includeClient && !ensureClient())) {
            return false;
        }
        const std::array supported{netClientList(), netActiveWindow(), netCloseWindow()};
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, owner_, supportingWmCheck(),
                            XCB_ATOM_WINDOW, 32, 1, &owner_);
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, root_, netSupported(),
                            XCB_ATOM_ATOM, 32, static_cast<std::uint32_t>(supported.size()),
                            supported.data());
        const auto clientCount = includeClient ? 1U : 0U;
        const auto *clients = includeClient ? &client_ : nullptr;
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, root_, netClientList(),
                            XCB_ATOM_WINDOW, 32, clientCount, clients);
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, root_, supportingWmCheck(),
                            XCB_ATOM_WINDOW, 32, 1, &owner_);
        return xcb_flush(connection_) > 0 && xcb_connection_has_error(connection_) == 0;
    }

    [[nodiscard]] bool withdraw() {
        if (!healthy()) {
            return false;
        }
        const auto deleted = xcb_delete_property_checked(connection_, root_, supportingWmCheck());
        ProtocolError error{xcb_request_check(connection_, deleted)};
        return !error && xcb_flush(connection_) > 0 && xcb_connection_has_error(connection_) == 0;
    }

  private:
    [[nodiscard]] xcb_atom_t namedAtom(std::string_view name) const {
        return intern(connection_, name);
    }

    [[nodiscard]] xcb_atom_t supportingWmCheck() const {
        return namedAtom("_NET_SUPPORTING_WM_CHECK");
    }
    [[nodiscard]] xcb_atom_t netSupported() const { return namedAtom("_NET_SUPPORTED"); }
    [[nodiscard]] xcb_atom_t netClientList() const { return namedAtom("_NET_CLIENT_LIST"); }
    [[nodiscard]] xcb_atom_t netActiveWindow() const { return namedAtom("_NET_ACTIVE_WINDOW"); }
    [[nodiscard]] xcb_atom_t netCloseWindow() const { return namedAtom("_NET_CLOSE_WINDOW"); }

    [[nodiscard]] bool ensureOwner() {
        if (owner_ != XCB_WINDOW_NONE) {
            return true;
        }
        owner_ = xcb_generate_id(connection_);
        const auto created =
            xcb_create_window_checked(connection_, depth_, owner_, root_, 0, 0, 1, 1, 0,
                                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual_, 0U, nullptr);
        ProtocolError error{xcb_request_check(connection_, created)};
        return !error;
    }

    [[nodiscard]] bool ensureClient() {
        if (client_ != XCB_WINDOW_NONE) {
            return true;
        }
        client_ = xcb_generate_id(connection_);
        const auto created =
            xcb_create_window_checked(connection_, depth_, client_, root_, 20, 20, 200, 120, 0,
                                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual_, 0U, nullptr);
        ProtocolError createError{xcb_request_check(connection_, created)};
        if (createError) {
            return false;
        }
        constexpr std::string_view title{"Prismdrake controlled task fixture"};
        const auto utf8 = namedAtom("UTF8_STRING");
        const auto netName = namedAtom("_NET_WM_NAME");
        constexpr std::array windowClass{'p', 'r', 'i', 's', 'm', 'd', 'r', 'a', 'k', 'e', '\0',
                                         'P', 'r', 'i', 's', 'm', 'd', 'r', 'a', 'k', 'e', '\0'};
        if (utf8 == XCB_ATOM_NONE || netName == XCB_ATOM_NONE) {
            return false;
        }
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client_, netName, utf8, 8,
                            static_cast<std::uint32_t>(title.size()), title.data());
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, client_, XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(windowClass.size()),
                            windowClass.data());
        xcb_map_window(connection_, client_);
        return xcb_connection_has_error(connection_) == 0;
    }

    xcb_connection_t *connection_{nullptr};
    xcb_window_t root_{XCB_WINDOW_NONE};
    xcb_window_t owner_{XCB_WINDOW_NONE};
    xcb_window_t client_{XCB_WINDOW_NONE};
    xcb_visualid_t visual_{XCB_NONE};
    std::uint8_t depth_{0U};
};

class NestedXServer final {
  public:
    [[nodiscard]] bool start(const char *executable) {
        if (executable == nullptr || *executable == '\0') {
            return false;
        }
        process_.setProgram(QString::fromLocal8Bit(executable));
        process_.setArguments({QStringLiteral("-displayfd"), QStringLiteral("1"),
                               QStringLiteral("-screen"), QStringLiteral("0"),
                               QStringLiteral("640x480x24"), QStringLiteral("-fp"),
                               QStringLiteral("built-ins"), QStringLiteral("-noreset"),
                               QStringLiteral("-nolisten"), QStringLiteral("tcp")});
        process_.start();
        if (!process_.waitForStarted(3000) || !process_.waitForReadyRead(3000)) {
            stop();
            return false;
        }
        const auto displayNumber = process_.readLine().trimmed();
        bool numeric = !displayNumber.isEmpty();
        for (const auto character : displayNumber) {
            numeric = numeric && character >= '0' && character <= '9';
        }
        if (!numeric) {
            stop();
            return false;
        }
        display_ = QStringLiteral(":") + QString::fromLatin1(displayNumber);
        return true;
    }

    void stop() {
        if (process_.state() == QProcess::NotRunning) {
            return;
        }
        process_.terminate();
        if (!process_.waitForFinished(3000)) {
            process_.kill();
            (void)process_.waitForFinished(2000);
        }
    }

    ~NestedXServer() { stop(); }

    [[nodiscard]] std::string display() const { return display_.toStdString(); }

  private:
    QProcess process_;
    QString display_;
};

class ControlledClients final {
  public:
    explicit ControlledClients(const char *display) {
        int screenIndex = 0;
        connection_ = xcb_connect(display, &screenIndex);
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            return;
        }
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        for (int index = 0; index < screenIndex && screens.rem > 0; ++index) {
            xcb_screen_next(&screens);
        }
        if (screens.rem > 0 && screens.data != nullptr) {
            root_ = screens.data->root;
            visual_ = screens.data->root_visual;
            depth_ = screens.data->root_depth;
        }
    }

    ~ControlledClients() {
        for (const auto window : windows_) {
            if (window != XCB_WINDOW_NONE && connection_ != nullptr) {
                (void)xcb_destroy_window(connection_, window);
            }
        }
        if (connection_ != nullptr) {
            (void)xcb_flush(connection_);
            xcb_disconnect(connection_);
        }
    }

    [[nodiscard]] bool healthy() const noexcept {
        return connection_ != nullptr && root_ != XCB_WINDOW_NONE &&
               xcb_connection_has_error(connection_) == 0;
    }

    [[nodiscard]] bool createAndMap(std::string_view title) {
        if (!healthy()) {
            return false;
        }
        const auto utf8 = intern(connection_, "UTF8_STRING");
        const auto netName = intern(connection_, "_NET_WM_NAME");
        if (utf8 == XCB_ATOM_NONE || netName == XCB_ATOM_NONE) {
            return false;
        }
        const auto window = xcb_generate_id(connection_);
        const auto created =
            xcb_create_window_checked(connection_, depth_, window, root_, 40, 40, 300, 180, 0,
                                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual_, 0U, nullptr);
        ProtocolError createError{xcb_request_check(connection_, created)};
        if (createError) {
            return false;
        }
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, netName, utf8, 8,
                            static_cast<std::uint32_t>(title.size()), title.data());
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(title.size()),
                            title.data());
        constexpr std::array windowClass{'p', 'r', 'i', 's', 'm',  'd', 'r', 'a', 'k', 'e', '-',
                                         't', 'e', 's', 't', '\0', 'P', 'r', 'i', 's', 'm', 'd',
                                         'r', 'a', 'k', 'e', 'T',  'e', 's', 't', '\0'};
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(windowClass.size()),
                            windowClass.data());
        windows_.push_back(window);
        xcb_map_window(connection_, window);
        return xcb_connection_has_error(connection_) == 0;
    }

    [[nodiscard]] bool flush() const { return healthy() && xcb_flush(connection_) > 0; }

    [[nodiscard]] bool destroyAll() {
        if (!healthy()) {
            return false;
        }
        for (auto &window : windows_) {
            const auto destroyed = xcb_destroy_window_checked(connection_, window);
            ProtocolError error{xcb_request_check(connection_, destroyed)};
            if (error) {
                return false;
            }
            window = XCB_WINDOW_NONE;
        }
        return xcb_flush(connection_) > 0;
    }

  private:
    xcb_connection_t *connection_{nullptr};
    xcb_window_t root_{XCB_WINDOW_NONE};
    xcb_visualid_t visual_{XCB_NONE};
    std::uint8_t depth_{0U};
    std::vector<xcb_window_t> windows_;
};

[[nodiscard]] bool bothFixtureTitlesPresented(const tasks::TaskPresentationModel &presentation) {
    constexpr std::array expected{std::string_view{"Prismdrake task fixture one"},
                                  std::string_view{"Prismdrake task fixture two"}};
    std::array<bool, expected.size()> observed{};
    for (int row = 0; row < presentation.rowCount(); ++row) {
        const auto *task = presentation.taskAt(row);
        if (task == nullptr) {
            return false;
        }
        const auto title = task->title().toStdString();
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            observed[index] = observed[index] || title == expected[index];
        }
    }
    return observed[0] && observed[1];
}

TEST(TaskControllerX11IntegrationTest, OwnsOneEventPipelineAndFailsSoftWithoutAWindowManager) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr) {
        GTEST_SKIP() << "the no-WM ownership case runs only in the plain Xvfb lane";
    }
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ensureApplication();
    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    bool connectionLost = false;

    auto controller = TaskController::create(
        presentation, display,
        [&connectionLost](const foundation::Error &) { connectionLost = true; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);
    EXPECT_FALSE(controller.value()->terminated());
    EXPECT_FALSE(connectionLost);
    EXPECT_EQ(presentation.rowCount(), 0);
    runEventLoopFor(400);
    ASSERT_EQ(recoverable.size(), 1U);
    EXPECT_EQ(recoverable.front().code, foundation::ErrorCode::not_found);
    runEventLoopFor(50);
    EXPECT_EQ(recoverable.size(), 1U);
}

TEST(TaskControllerX11IntegrationTest, StartupOwnerAppearsDuringBoundedStabilizationEpoch) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr) {
        GTEST_SKIP() << "the controlled owner case runs only in the plain Xvfb lane";
    }
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ensureApplication();
    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    int shutdownCount = 0;
    auto controller = TaskController::create(
        presentation, display, [&shutdownCount](const foundation::Error &) { ++shutdownCount; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);
    ASSERT_FALSE(controller.value()->currentSnapshot());

    ControlledEwmhOwner owner{display};
    ASSERT_TRUE(owner.healthy());
    ASSERT_TRUE(owner.publish(false));
    ASSERT_TRUE(waitForPublication(
        presentation, [&controller] { return controller.value()->currentSnapshot() != nullptr; },
        1000));
    EXPECT_EQ(presentation.rowCount(), 0);
    EXPECT_TRUE(recoverable.empty());
    EXPECT_EQ(shutdownCount, 0);

    runEventLoopFor(400);
    EXPECT_TRUE(recoverable.empty());
    EXPECT_EQ(shutdownCount, 0);
}

TEST(TaskControllerX11IntegrationTest, RealEventStartsNewEpochAfterPreviousExhaustion) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr) {
        GTEST_SKIP() << "the controlled owner case runs only in the plain Xvfb lane";
    }
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ensureApplication();
    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    int shutdownCount = 0;
    auto controller = TaskController::create(
        presentation, display, [&shutdownCount](const foundation::Error &) { ++shutdownCount; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);
    runEventLoopFor(400);
    ASSERT_EQ(recoverable.size(), 1U);

    ControlledEwmhOwner owner{display};
    ASSERT_TRUE(owner.healthy());
    ASSERT_TRUE(owner.publish(false));
    ASSERT_TRUE(waitForPublication(
        presentation, [&controller] { return controller.value()->currentSnapshot() != nullptr; },
        1000));
    runEventLoopFor(400);
    EXPECT_EQ(recoverable.size(), 1U);
    EXPECT_EQ(shutdownCount, 0);
}

TEST(TaskControllerX11IntegrationTest, RejectsCheckedRequestWhileStaleSnapshotStabilizes) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr) {
        GTEST_SKIP() << "the controlled owner case runs only in the plain Xvfb lane";
    }
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ensureApplication();
    ControlledEwmhOwner owner{display};
    ASSERT_TRUE(owner.healthy());
    ASSERT_TRUE(owner.publish(true));

    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    std::vector<TaskRequestUpdate> outcomes;
    int shutdownCount = 0;
    auto controller = TaskController::create(
        presentation, display, [&shutdownCount](const foundation::Error &) { ++shutdownCount; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); },
        [&outcomes](const TaskRequestUpdate &update) { outcomes.push_back(update); });
    ASSERT_TRUE(controller);
    ASSERT_EQ(presentation.rowCount(), 1);
    ASSERT_TRUE(owner.withdraw());
    runEventLoopFor(5);

    ASSERT_NE(presentation.taskAt(0), nullptr);
    ASSERT_TRUE(presentation.taskAt(0)->requestClose());
    ASSERT_EQ(outcomes.size(), 1U);
    EXPECT_EQ(outcomes.front().outcome, x11::TaskRequestOutcome::deliveryRejected);
    ASSERT_EQ(recoverable.size(), 1U);
    EXPECT_EQ(recoverable.front().code, foundation::ErrorCode::cancelled);
    EXPECT_EQ(controller.value()->pendingRequestCount(), 0U);
    EXPECT_EQ(shutdownCount, 0);

    runEventLoopFor(400);
    ASSERT_EQ(recoverable.size(), 2U);
    EXPECT_EQ(recoverable[1].code, foundation::ErrorCode::not_found);
    ASSERT_TRUE(presentation.taskAt(0)->requestClose());
    ASSERT_EQ(outcomes.size(), 2U);
    EXPECT_EQ(outcomes.back().outcome, x11::TaskRequestOutcome::deliveryRejected);
    ASSERT_EQ(recoverable.size(), 3U);
    EXPECT_EQ(recoverable.back().code, foundation::ErrorCode::cancelled);
    EXPECT_EQ(controller.value()->pendingRequestCount(), 0U);

    ASSERT_TRUE(owner.publish(true));
    runEventLoopFor(400);
    EXPECT_EQ(recoverable.size(), 3U);
    EXPECT_EQ(shutdownCount, 0);
}

TEST(TaskControllerX11IntegrationTest, ConnectionLossCancelsRetryAndShutsDownExactlyOnce) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") != nullptr) {
        GTEST_SKIP() << "the nested transport case runs only in the plain Xvfb lane";
    }
    const char *xvfb = std::getenv("PRISMDRAKE_TEST_XVFB_EXECUTABLE");
    ASSERT_NE(xvfb, nullptr);
    ensureApplication();
    NestedXServer server;
    ASSERT_TRUE(server.start(xvfb));

    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    std::vector<foundation::Error> shutdown;
    auto controller = TaskController::create(
        presentation, server.display(),
        [&shutdown](const foundation::Error &error) { shutdown.push_back(error); },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);
    server.stop();
    runEventLoopFor(1000);

    EXPECT_TRUE(controller.value()->terminated());
    ASSERT_EQ(shutdown.size(), 1U);
    EXPECT_EQ(shutdown.front().code, foundation::ErrorCode::io_error);
    EXPECT_TRUE(recoverable.empty());
    runEventLoopFor(400);
    EXPECT_EQ(shutdown.size(), 1U);
    EXPECT_TRUE(recoverable.empty());
}

TEST(TaskControllerX11IntegrationTest, StabilizesBackToBackOpenboxMapsAndRemovesDestroyedClients) {
    if (std::getenv("PRISMDRAKE_TEST_EXPECT_EWMH_WM") == nullptr) {
        GTEST_SKIP() << "the map-burst case requires the Openbox harness lane";
    }
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    ensureApplication();
    tasks::TaskPresentationModel presentation;
    std::vector<foundation::Error> recoverable;
    bool connectionLost = false;
    auto controller = TaskController::create(
        presentation, display,
        [&connectionLost](const foundation::Error &) { connectionLost = true; },
        [&recoverable](const foundation::Error &error) { recoverable.push_back(error); });
    ASSERT_TRUE(controller);

    ControlledClients clients{display};
    ASSERT_TRUE(clients.healthy());
    ASSERT_TRUE(clients.createAndMap("Prismdrake task fixture one"));
    ASSERT_TRUE(clients.createAndMap("Prismdrake task fixture two"));
    ASSERT_TRUE(clients.flush());
    ASSERT_TRUE(waitForRows(presentation, 2, 3000))
        << "row_count=" << presentation.rowCount() << " recoverable_count=" << recoverable.size();
    EXPECT_TRUE(bothFixtureTitlesPresented(presentation));
    EXPECT_FALSE(connectionLost);
    EXPECT_TRUE(recoverable.empty());
    runEventLoopFor(400);
    EXPECT_EQ(presentation.rowCount(), 2);
    EXPECT_TRUE(recoverable.empty());

    ASSERT_TRUE(clients.destroyAll());
    ASSERT_TRUE(waitForRows(presentation, 0, 3000)) << "row_count=" << presentation.rowCount();
    EXPECT_FALSE(connectionLost);
}

} // namespace
} // namespace prismdrake::shell::taskcontroller
