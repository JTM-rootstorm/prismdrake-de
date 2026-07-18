#include <cstdlib>
#include <string_view>

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

} // namespace

int main(int argc, char **argv) {
    if (!validSessionEnvironment()) {
        return 120;
    }
    if (argc == 1 && std::string_view{argv[0]} == "prismdrake-shell") {
        return 0;
    }
    if (argc == 3 && std::string_view{argv[0]} == "prismdrake-settingsd" &&
        std::string_view{argv[1]} == "--foreground" && std::string_view{argv[2]} == "--safe-mode") {
        return 0;
    }
    return 121;
}
