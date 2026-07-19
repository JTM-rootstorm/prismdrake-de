#include "ShellCtl.hpp"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int main(int argc, char **argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto parsed = prismdrake::shellctl::parseCommand(arguments);
    if (!parsed.valid()) {
        std::cerr << prismdrake::shellctl::usageText();
        return static_cast<int>(prismdrake::shellctl::ExitStatus::usage_error);
    }
    const auto status = prismdrake::shellctl::invoke(parsed.command);
    if (status != prismdrake::shellctl::ExitStatus::success) {
        std::cerr << "prismdrake-shellctl: The typed control request failed (status "
                  << static_cast<int>(status) << ").\n";
    }
    return static_cast<int>(status);
}
