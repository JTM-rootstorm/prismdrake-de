#include "ExitStatus.hpp"
#include "SessionEnvironment.hpp"
#include "SessionOptions.hpp"
#include "SessionRuntime.hpp"
#include "SessionSupervisor.hpp"
#include "X11Connection.hpp"
#include "XdgPaths.hpp"

#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace foundation = prismdrake::foundation;
using foundation::ExitStatus;

[[nodiscard]] int run(int argc, char **argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const prismdrake::session::SessionPathDefaults packagedPaths{
        PRISMDRAKE_SESSION_DEFAULT_SETTINGSD_EXECUTABLE,
        PRISMDRAKE_SESSION_DEFAULT_SHELL_EXECUTABLE};
    auto options = prismdrake::session::parseSessionOptions(arguments, packagedPaths);
    if (!options) {
        std::cerr << "prismdrake-session: " << options.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(options.error()));
    }
    if (options.value().action == prismdrake::session::SessionAction::show_help) {
        std::cout << prismdrake::session::sessionHelpText();
        return foundation::processExitCode(ExitStatus::success);
    }
    if (options.value().action == prismdrake::session::SessionAction::show_version) {
        std::cout << prismdrake::session::sessionVersionText();
        return foundation::processExitCode(ExitStatus::success);
    }
    if (!options.value().runtime) {
        std::cerr << "prismdrake-session: Runtime options are unavailable.\n";
        return foundation::processExitCode(ExitStatus::unavailable);
    }

    auto environment = prismdrake::session::prepareCurrentSessionEnvironment();
    if (!environment) {
        std::cerr << "prismdrake-session: " << environment.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(environment.error()));
    }
    auto display = prismdrake::x11::probeUsableDisplay(environment.value().display);
    if (!display) {
        std::cerr << "prismdrake-session: " << display.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(display.error()));
    }
    auto paths = foundation::resolveCurrentProcessXdgPaths();
    if (!paths) {
        std::cerr << "prismdrake-session: " << paths.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(paths.error()));
    }
    auto signals = prismdrake::session::installSessionSignalHandlers();
    if (!signals) {
        std::cerr << "prismdrake-session: " << signals.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(signals.error()));
    }
    auto runtime = prismdrake::session::SessionRuntime::prepare(paths.value());
    if (!runtime) {
        std::cerr << "prismdrake-session: " << runtime.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(runtime.error()));
    }

    auto platform = prismdrake::session::makeProductionSessionSupervisorPlatform(
        std::move(*options.value().runtime), std::move(environment).value(), runtime.value());
    auto supervisor = prismdrake::session::SessionSupervisor::create(*platform);
    if (!supervisor) {
        std::cerr << "prismdrake-session: " << supervisor.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(supervisor.error()));
    }
    auto outcome = supervisor.value().run();
    auto cleanup = runtime.value().cleanup();
    if (!cleanup) {
        std::cerr << "prismdrake-session: " << cleanup.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(cleanup.error()));
    }
    if (!outcome) {
        std::cerr << "prismdrake-session: " << outcome.error().message << '\n';
        return foundation::processExitCode(foundation::exitStatusFor(outcome.error()));
    }
    return foundation::processExitCode(
        outcome.value() == prismdrake::session::SessionTermination::clean ? ExitStatus::success
                                                                          : ExitStatus::cancelled);
}

} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &) {
        std::cerr << "prismdrake-session: An unexpected bounded startup failure occurred.\n";
    } catch (...) {
        std::cerr << "prismdrake-session: An unexpected internal failure occurred.\n";
    }
    return foundation::processExitCode(foundation::ExitStatus::general_failure);
}
