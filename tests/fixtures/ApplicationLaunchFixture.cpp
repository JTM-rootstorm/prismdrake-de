#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

extern char **environ;

namespace {

constexpr std::uint32_t fixtureMagic = 0x50444C46U;

void writeUnsigned(std::ofstream &stream, std::uint64_t value) {
    stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

void writeString(std::ofstream &stream, std::string_view value) {
    writeUnsigned(stream, value.size());
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr || environ == nullptr) {
        return 64;
    }

    std::vector<std::string> environment;
    for (char **entry = environ; *entry != nullptr; ++entry) {
        environment.emplace_back(*entry);
    }

    std::error_code error;
    const auto workingDirectory = std::filesystem::current_path(error);
    if (error) {
        return 65;
    }

    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output) {
        return 66;
    }
    output.write(reinterpret_cast<const char *>(&fixtureMagic), sizeof(fixtureMagic));
    writeUnsigned(output, static_cast<std::uint64_t>(argc));
    for (int index = 0; index < argc; ++index) {
        writeString(output, argv[index]);
    }
    writeString(output, workingDirectory.native());
    writeUnsigned(output, environment.size());
    for (const auto &entry : environment) {
        writeString(output, entry);
    }
    output.flush();
    return output ? 0 : 67;
}
