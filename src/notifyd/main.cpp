#include "NotificationService.hpp"
#include "ServiceOptions.hpp"

#include "ExitStatus.hpp"
#include "MonotonicClock.hpp"
#include "XdgPaths.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

extern "C" void requestStop(int) { stopRequested = 1; }

} // namespace

[[nodiscard]] int runMain(int argc, char **argv) {
    using prismdrake::foundation::ExitStatus;
    using prismdrake::foundation::processExitCode;
    using namespace prismdrake::notifyd;

    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    auto options =
        parseServiceOptions(arguments, prismdrake::foundation::currentProcessXdgEnvironment());
    if (!options) {
        std::cerr << "prismdrake-notifyd: " << options.error().message << '\n';
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

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    auto reconnectDelay = std::chrono::milliseconds{100};
    constexpr auto maximumReconnectDelay = std::chrono::milliseconds{2000};
    while (stopRequested == 0) {
        auto authority = NotificationAuthority::create(
            std::make_shared<prismdrake::foundation::SystemMonotonicClock>(),
            options.value().authority);
        if (!authority) {
            std::cerr << "prismdrake-notifyd: " << authority.error().message << '\n';
            return processExitCode(prismdrake::foundation::exitStatusFor(authority.error()));
        }
        auto epoch = runServiceEpoch(std::move(authority).value(), stopRequested);
        if (!epoch) {
            std::cerr << "prismdrake-notifyd: " << epoch.error().message << '\n';
            return processExitCode(prismdrake::foundation::exitStatusFor(epoch.error()));
        }
        if (epoch.value() == ServiceEpochOutcome::stopped) {
            return processExitCode(ExitStatus::success);
        }
        if (epoch.value() == ServiceEpochOutcome::name_conflict) {
            std::cerr << "prismdrake-notifyd: A notification service name is already owned.\n";
            return processExitCode(ExitStatus::unavailable);
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
        std::cerr << "prismdrake-notifyd: An unexpected bounded service failure occurred.\n";
    } catch (...) {
        std::cerr << "prismdrake-notifyd: An unexpected internal failure occurred.\n";
    }
    return prismdrake::foundation::processExitCode(
        prismdrake::foundation::ExitStatus::general_failure);
}
