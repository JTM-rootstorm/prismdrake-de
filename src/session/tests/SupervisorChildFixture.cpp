#include "SessionReadinessProtocol.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

[[nodiscard]] bool environmentEquals(const char *name, std::string_view expected) {
    const char *value = std::getenv(name);
    return value != nullptr && std::string_view{value} == expected;
}

[[nodiscard]] bool validSessionEnvironment() {
    return environmentEquals("XDG_CURRENT_DESKTOP", "Prismdrake") &&
           environmentEquals("XDG_SESSION_DESKTOP", "prismdrake") &&
           environmentEquals("DESKTOP_SESSION", "prismdrake") &&
           environmentEquals("SUPERVISOR_FIXTURE", "exact-value");
}

[[nodiscard]] bool publishShellReadiness() {
    const char *value =
        std::getenv(prismdrake::foundation::sessionReadinessDescriptorEnvironment.data());
    if (value == nullptr) {
        return false;
    }
    const std::string_view encoded{value};
    int descriptor = -1;
    const auto parsed =
        std::from_chars(encoded.data(), encoded.data() + encoded.size(), descriptor);
    if (encoded.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != encoded.data() + encoded.size() || descriptor < 3) {
        return false;
    }
    const std::string_view mode = [] {
        const char *configured = std::getenv("SUPERVISOR_FIXTURE_READY_MODE");
        return configured == nullptr ? std::string_view{} : std::string_view{configured};
    }();
    if (mode == "close") {
        static_cast<void>(::close(descriptor));
        return true;
    }
    const auto message =
        mode == "wrong-message"
            ? static_cast<std::uint64_t>(prismdrake::foundation::sessionReadinessMessage ^ 0xFFU)
            : prismdrake::foundation::sessionReadinessMessage;
    ssize_t sent = -1;
    do {
        sent = ::write(descriptor, &message, sizeof(message));
    } while (sent < 0 && errno == EINTR);
    static_cast<void>(::close(descriptor));
    return sent == static_cast<ssize_t>(sizeof(message));
}

} // namespace

int main(int argc, char **argv) {
    if (!validSessionEnvironment()) {
        return 120;
    }
    if (argc == 1 && std::string_view{argv[0]} == "prismdrake-shell") {
        return publishShellReadiness() ? 0 : 122;
    }
    if (argc == 3 && std::string_view{argv[0]} == "prismdrake-settingsd" &&
        std::string_view{argv[1]} == "--foreground" && std::string_view{argv[2]} == "--safe-mode") {
        return 0;
    }
    return 121;
}
