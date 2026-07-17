#include "SettingsService.hpp"

#include "ExitStatus.hpp"
#include "ServiceOptions.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

extern "C" void requestStop(int) { stopRequested = 1; }

} // namespace

[[nodiscard]] int runMain(int argc, char **argv) {
    using prismdrake::foundation::ExitStatus;
    using prismdrake::foundation::processExitCode;
    using namespace prismdrake::settingsd;

    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const ServicePathDefaults defaults{PRISMDRAKE_PACKAGED_CONFIGURATION,
                                       PRISMDRAKE_PACKAGED_THEME_DIRECTORY};
    auto options = parseServiceOptions(std::span<const std::string_view>{arguments}, defaults);
    if (!options) {
        std::cerr << "prismdrake-settingsd: " << options.error().message << '\n';
        return processExitCode(prismdrake::foundation::exitStatusFor(options.error()));
    }
    if (options.value().action == ServiceAction::show_help) {
        std::cout << serviceHelpText();
        return processExitCode(ExitStatus::success);
    }
    if (options.value().action == ServiceAction::show_version) {
        std::cout << serviceVersionText();
        return processExitCode(ExitStatus::success);
    }
    auto serviceOptions = std::move(options).value();
    if (!serviceOptions.runtime) {
        std::cerr << "prismdrake-settingsd: Runtime options are unavailable.\n";
        return processExitCode(ExitStatus::unavailable);
    }
    auto runtimeOptions = std::move(serviceOptions.runtime).value();

    const auto runtimeBoundary =
        prismdrake::foundation::validateCurrentProcessRuntimeDirectory(runtimeOptions.xdgPaths);
    if (!runtimeBoundary) {
        std::cerr << "prismdrake-settingsd: " << runtimeBoundary.error().message << '\n';
        return processExitCode(prismdrake::foundation::exitStatusFor(runtimeBoundary.error()));
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    auto reconnectDelay = std::chrono::milliseconds{100};
    constexpr auto maximumReconnectDelay = std::chrono::milliseconds{2000};
    while (stopRequested == 0) {
        auto epoch = runServiceEpoch(runtimeOptions.settingsEngine, stopRequested);
        if (!epoch) {
            std::cerr << "prismdrake-settingsd: " << epoch.error().message << '\n';
            return processExitCode(prismdrake::foundation::exitStatusFor(epoch.error()));
        }
        if (epoch.value() == ServiceEpochOutcome::stopped) {
            return processExitCode(ExitStatus::success);
        }
        std::this_thread::sleep_for(reconnectDelay);
        reconnectDelay = std::min(reconnectDelay * 2, maximumReconnectDelay);
    }
    return processExitCode(ExitStatus::success);
}

int main(int argc, char **argv) {
    try {
        return runMain(argc, argv);
    } catch (const std::exception &) {
        std::cerr << "prismdrake-settingsd: An unexpected bounded startup failure occurred.\n";
    } catch (...) {
        std::cerr << "prismdrake-settingsd: An unexpected internal failure occurred.\n";
    }
    return prismdrake::foundation::processExitCode(
        prismdrake::foundation::ExitStatus::general_failure);
}
