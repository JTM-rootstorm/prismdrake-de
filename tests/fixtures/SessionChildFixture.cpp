#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <unistd.h>

namespace {

[[nodiscard]] int parseInt(const char *value) {
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0L || parsed > 255L) {
        return -1;
    }
    return static_cast<int>(parsed);
}

[[nodiscard]] bool markReady(const char *path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "ready\n";
    return output.good();
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::string_view{argv[1]} == "--exit") {
        const int code = parseInt(argv[2]);
        return code < 0 ? 125 : code;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--wait") {
        for (;;) {
            ::pause();
        }
    }
    if (argc == 3 && std::string_view{argv[1]} == "--ignore-term") {
        if (::signal(SIGTERM, SIG_IGN) == SIG_ERR || !markReady(argv[2])) {
            return 124;
        }
        for (;;) {
            ::pause();
        }
    }
    if (argc == 6 && std::string_view{argv[1]} == "--verify") {
        const char *value = std::getenv(argv[2]);
        if (value == nullptr || std::string_view{value} != argv[3] ||
            std::getenv(argv[4]) != nullptr || std::string_view{argv[5]} != "literal;$(false)") {
            return 123;
        }
        return 0;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--sleep-exit") {
        const int milliseconds = parseInt(argv[2]);
        if (milliseconds < 0) {
            return 122;
        }
        ::usleep(static_cast<useconds_t>(milliseconds) * 1000U);
        return 17;
    }
    return 121;
}
