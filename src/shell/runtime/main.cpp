#include "ShellRuntime.hpp"

#include "ExitStatus.hpp"
#include "ShellRuntimeOptions.hpp"
#include "TerminationSignalBridge.hpp"

#include <QGuiApplication>

#include <exception>
#include <iostream>

namespace {

[[nodiscard]] int runMain(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("prismdrake-shell"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("prismdrake.org"));

    auto termination = prismdrake::shell::runtime::TerminationSignalBridge::create(
        [&application]() { application.quit(); });
    if (!termination) {
        std::cerr << "prismdrake-shell: " << termination.error().message << '\n';
        return prismdrake::foundation::processExitCode(
            prismdrake::foundation::exitStatusFor(termination.error()));
    }

    auto options = prismdrake::shell::runtime::currentShellRuntimeOptions();
    if (!options) {
        std::cerr << "prismdrake-shell: " << options.error().message << '\n';
        return prismdrake::foundation::processExitCode(
            prismdrake::foundation::exitStatusFor(options.error()));
    }
    auto runtime = prismdrake::shell::runtime::ShellRuntime::create(std::move(options).value());
    if (!runtime) {
        std::cerr << "prismdrake-shell: " << runtime.error().message << '\n';
        return prismdrake::foundation::processExitCode(
            prismdrake::foundation::exitStatusFor(runtime.error()));
    }
    auto started = runtime.value()->start();
    if (!started) {
        std::cerr << "prismdrake-shell: " << started.error().message << '\n';
        return prismdrake::foundation::processExitCode(
            prismdrake::foundation::exitStatusFor(started.error()));
    }
    return application.exec();
}

} // namespace

int main(int argc, char **argv) {
    try {
        return runMain(argc, argv);
    } catch (const std::exception &) {
        std::cerr << "prismdrake-shell: An unexpected bounded runtime failure occurred.\n";
    } catch (...) {
        std::cerr << "prismdrake-shell: An unexpected internal failure occurred.\n";
    }
    return prismdrake::foundation::processExitCode(
        prismdrake::foundation::ExitStatus::general_failure);
}
