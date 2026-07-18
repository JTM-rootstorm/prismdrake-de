#include <xcb/xcb.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void requestStop(int) { stop_requested = 1; }

[[nodiscard]] xcb_atom_t intern(xcb_connection_t *connection, std::string_view name) {
    const auto cookie =
        xcb_intern_atom(connection, 0, static_cast<std::uint16_t>(name.size()), name.data());
    auto *reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (reply == nullptr) {
        return XCB_ATOM_NONE;
    }
    const auto atom = reply->atom;
    std::free(reply);
    return atom;
}

[[nodiscard]] int run(std::string_view instance) {
    const std::string_view title = instance == "one"   ? "Prismdrake Demo App One"
                                   : instance == "two" ? "Prismdrake Demo App Two"
                                                       : std::string_view{};
    if (title.empty()) {
        return 64;
    }

    int screen_index = 0;
    xcb_connection_t *connection = xcb_connect(nullptr, &screen_index);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0) {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
        return 69;
    }

    const auto *setup = xcb_get_setup(connection);
    if (setup == nullptr) {
        xcb_disconnect(connection);
        return 69;
    }
    auto screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_index && screens.rem > 0; ++index) {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr) {
        xcb_disconnect(connection);
        return 69;
    }
    const auto *screen = screens.data;
    const auto window = xcb_generate_id(connection);
    const std::array<std::uint32_t, 2> values{screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    const auto created =
        xcb_create_window_checked(connection, screen->root_depth, window, screen->root, 80, 80, 360,
                                  220, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values.data());
    if (auto *error = xcb_request_check(connection, created); error != nullptr) {
        std::free(error);
        xcb_disconnect(connection);
        return 69;
    }

    const auto utf8 = intern(connection, "UTF8_STRING");
    const auto net_name = intern(connection, "_NET_WM_NAME");
    const auto net_wm_pid = intern(connection, "_NET_WM_PID");
    const auto protocols = intern(connection, "WM_PROTOCOLS");
    const auto delete_window = intern(connection, "WM_DELETE_WINDOW");
    if (utf8 == XCB_ATOM_NONE || net_name == XCB_ATOM_NONE || net_wm_pid == XCB_ATOM_NONE ||
        protocols == XCB_ATOM_NONE || delete_window == XCB_ATOM_NONE) {
        xcb_disconnect(connection);
        return 69;
    }
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, net_name, utf8, 8,
                        static_cast<std::uint32_t>(title.size()), title.data());
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(title.size()), title.data());
    const auto process_id = static_cast<std::uint32_t>(::getpid());
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, net_wm_pid, XCB_ATOM_CARDINAL,
                        32, 1, &process_id);
    constexpr std::array wm_class{'p', 'r', 'i', 's', 'm',  'd', 'r', 'a', 'k', 'e', '-',
                                  'd', 'e', 'm', 'o', '\0', 'P', 'r', 'i', 's', 'm', 'd',
                                  'r', 'a', 'k', 'e', 'D',  'e', 'm', 'o', '\0'};
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(wm_class.size()),
                        wm_class.data());
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, protocols, XCB_ATOM_ATOM, 32, 1,
                        &delete_window);
    xcb_map_window(connection, window);
    if (xcb_flush(connection) <= 0) {
        xcb_disconnect(connection);
        return 69;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{70};
    while (stop_requested == 0 && std::chrono::steady_clock::now() < deadline &&
           xcb_connection_has_error(connection) == 0) {
        while (auto *event = xcb_poll_for_event(connection)) {
            const auto type = event->response_type & ~0x80U;
            bool close = type == XCB_DESTROY_NOTIFY;
            if (type == XCB_CLIENT_MESSAGE) {
                const auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                close = message->type == protocols && message->data.data32[0] == delete_window;
            }
            std::free(event);
            if (close) {
                xcb_disconnect(connection);
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
    xcb_disconnect(connection);
    return stop_requested == 0 ? 70 : 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || argv == nullptr || std::string_view{argv[1]} != "--instance") {
        return 64;
    }
    return run(argv[2]);
}
