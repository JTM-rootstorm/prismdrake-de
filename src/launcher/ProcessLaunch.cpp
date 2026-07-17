#include "ProcessLaunch.hpp"

#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool containsNull(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool addWithin(std::size_t &total, std::size_t amount, std::size_t limit) noexcept {
    if (amount > limit - total) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] Result<std::filesystem::path> workingDirectory(const DesktopEntry &entry,
                                                             const ProcessLaunchContext &context) {
    const auto defaultNative = context.defaultWorkingDirectory.native();
    if (context.defaultWorkingDirectory.empty() || !context.defaultWorkingDirectory.is_absolute() ||
        containsNull(defaultNative)) {
        return failure<std::filesystem::path>(
            ErrorCode::invalid_argument, "The default launch working directory is invalid.",
            "Provide an explicit NUL-free absolute default working directory.");
    }
    if (defaultNative.size() > maximumProcessLaunchWorkingDirectoryBytes) {
        return failure<std::filesystem::path>(
            ErrorCode::too_large, "The default launch working directory exceeds its byte limit.",
            "Provide a smaller explicit absolute default working directory.");
    }

    auto resolvedDefault = context.defaultWorkingDirectory.lexically_normal();
    if (resolvedDefault.empty() || !resolvedDefault.is_absolute() ||
        resolvedDefault.native().size() > maximumProcessLaunchWorkingDirectoryBytes) {
        return failure<std::filesystem::path>(
            ErrorCode::invalid_argument, "The default launch working directory is invalid.",
            "Provide an explicit normalized absolute default working directory.");
    }
    if (!entry.path || entry.path->empty()) {
        return Result<std::filesystem::path>::success(std::move(resolvedDefault));
    }
    if (containsNull(*entry.path)) {
        return failure<std::filesystem::path>(
            ErrorCode::invalid_argument, "The desktop-entry working directory is invalid.",
            "Use a NUL-free Path value or omit it to use the explicit default.");
    }
    if (entry.path->size() > maximumProcessLaunchWorkingDirectoryBytes) {
        return failure<std::filesystem::path>(
            ErrorCode::too_large, "The desktop-entry working directory exceeds its byte limit.",
            "Use a smaller Path value or omit it to use the explicit default.");
    }

    std::filesystem::path selected{*entry.path};
    if (!selected.is_absolute()) {
        selected = resolvedDefault / selected;
    }
    selected = selected.lexically_normal();
    if (selected.empty() || !selected.is_absolute()) {
        return failure<std::filesystem::path>(
            ErrorCode::invalid_argument, "The desktop-entry working directory is invalid.",
            "Use a Path that resolves against the explicit absolute default directory.");
    }
    if (selected.native().size() > maximumProcessLaunchWorkingDirectoryBytes) {
        return failure<std::filesystem::path>(
            ErrorCode::too_large,
            "The resolved desktop-entry working directory exceeds its byte limit.",
            "Use a smaller Path value or explicit default working directory.");
    }
    return Result<std::filesystem::path>::success(std::move(selected));
}

[[nodiscard]] Result<void> validateArgument(std::string_view argument) {
    if (containsNull(argument)) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "A process launch argument contains a null byte.",
                                      "Use only NUL-free expanded and configured arguments."});
    }
    if (argument.size() > maximumProcessLaunchArgumentBytes) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "A process launch argument exceeds its byte limit.",
                                      "Use smaller expanded and configured arguments."});
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validateInvocation(const DesktopExecInvocation &invocation) {
    if (invocation.argv.empty() || invocation.argv.front().empty()) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "The process launch argument vector has no executable.",
                                      "Provide an expanded invocation with a nonempty argv[0]."});
    }
    if (invocation.argv.size() > maximumProcessLaunchArguments) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "The process launch argument vector has too many entries.",
                                      "Use fewer expanded arguments."});
    }
    for (const auto &argument : invocation.argv) {
        auto validated = validateArgument(argument);
        if (!validated) {
            return validated;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::vector<std::string>>
normalizedEnvironment(const std::vector<std::string> &environment,
                      const std::filesystem::path &selectedWorkingDirectory) {
    if (environment.size() > maximumProcessLaunchEnvironmentEntries) {
        return failure<std::vector<std::string>>(
            ErrorCode::too_large, "The process launch environment has too many entries.",
            "Provide a smaller explicit environment snapshot.");
    }

    std::size_t inputBytes = 0U;
    std::unordered_set<std::string_view> explicitNames;
    explicitNames.reserve(environment.size());
    for (const auto &entry : environment) {
        if (entry.empty() || containsNull(entry)) {
            return failure<std::vector<std::string>>(
                ErrorCode::invalid_environment, "The process launch environment is malformed.",
                "Provide nonempty NUL-free NAME=value entries.");
        }
        const auto separator = entry.find('=');
        if (separator == std::string::npos || separator == 0U) {
            return failure<std::vector<std::string>>(
                ErrorCode::invalid_environment, "The process launch environment is malformed.",
                "Provide nonempty NUL-free NAME=value entries.");
        }
        const std::string_view name{entry.data(), separator};
        if (name != "PWD" && !explicitNames.insert(name).second) {
            return failure<std::vector<std::string>>(
                ErrorCode::invalid_environment,
                "The process launch environment contains a duplicate variable name.",
                "Provide each explicit environment variable at most once.");
        }
        if (entry.size() > maximumProcessLaunchEnvironmentEntryBytes) {
            return failure<std::vector<std::string>>(
                ErrorCode::too_large, "A process launch environment entry exceeds its byte limit.",
                "Provide smaller explicit environment values.");
        }
        if (!addWithin(inputBytes, entry.size() + 1U, maximumProcessLaunchEnvironmentBytes)) {
            return failure<std::vector<std::string>>(
                ErrorCode::too_large, "The process launch environment exceeds its byte limit.",
                "Provide a smaller explicit environment snapshot.");
        }
    }

    const std::string pwdEntry = "PWD=" + selectedWorkingDirectory.native();
    if (pwdEntry.size() > maximumProcessLaunchEnvironmentEntryBytes) {
        return failure<std::vector<std::string>>(
            ErrorCode::too_large, "The process launch PWD entry exceeds its byte limit.",
            "Use a smaller explicit working directory.");
    }

    std::vector<std::string> normalized;
    normalized.reserve(environment.size() + 1U);
    bool wrotePwd = false;
    for (const auto &entry : environment) {
        const auto separator = entry.find('=');
        if (entry.substr(0U, separator) == "PWD") {
            if (!wrotePwd) {
                normalized.push_back(pwdEntry);
                wrotePwd = true;
            }
            continue;
        }
        normalized.push_back(entry);
    }
    if (!wrotePwd) {
        if (normalized.size() >= maximumProcessLaunchEnvironmentEntries) {
            return failure<std::vector<std::string>>(
                ErrorCode::too_large, "The normalized process environment has too many entries.",
                "Reserve one environment entry for the selected working directory.");
        }
        normalized.push_back(pwdEntry);
    }

    std::size_t normalizedBytes = 0U;
    for (const auto &entry : normalized) {
        if (!addWithin(normalizedBytes, entry.size() + 1U, maximumProcessLaunchEnvironmentBytes)) {
            return failure<std::vector<std::string>>(
                ErrorCode::too_large,
                "The normalized process launch environment exceeds its byte limit.",
                "Provide a smaller environment snapshot or working directory.");
        }
    }
    return Result<std::vector<std::string>>::success(std::move(normalized));
}

[[nodiscard]] Result<void> validateFinalEnvelope(const ProcessLaunchPlan &plan) {
    if (plan.argv.empty() || plan.argv.front().empty() ||
        plan.argv.size() > maximumProcessLaunchArguments) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "The final process launch argument vector is too large.",
                                      "Use fewer terminal-prefix or application arguments."});
    }

    std::size_t envelopeBytes = 0U;
    constexpr std::size_t nullPointerCount = 2U;
    const auto pointerCount = plan.argv.size() + plan.environment.size() + nullPointerCount;
    if (pointerCount > std::numeric_limits<std::size_t>::max() / sizeof(char *) ||
        !addWithin(envelopeBytes, pointerCount * sizeof(char *),
                   maximumProcessLaunchEnvelopeBytes)) {
        return Result<void>::failure({ErrorCode::too_large,
                                      "The final process launch envelope exceeds its byte limit.",
                                      "Use fewer arguments and environment entries."});
    }

    for (const auto &argument : plan.argv) {
        auto validated = validateArgument(argument);
        if (!validated) {
            return validated;
        }
        if (!addWithin(envelopeBytes, argument.size() + 1U, maximumProcessLaunchEnvelopeBytes)) {
            return Result<void>::failure(
                {ErrorCode::too_large, "The final process launch envelope exceeds its byte limit.",
                 "Use smaller arguments or environment values."});
        }
    }
    for (const auto &entry : plan.environment) {
        if (!addWithin(envelopeBytes, entry.size() + 1U, maximumProcessLaunchEnvelopeBytes)) {
            return Result<void>::failure(
                {ErrorCode::too_large, "The final process launch envelope exceeds its byte limit.",
                 "Use smaller arguments or environment values."});
        }
    }
    return Result<void>::success();
}

} // namespace

Result<ProcessLaunchPlan> makeProcessLaunchPlan(const DesktopEntry &entry,
                                                const DesktopExecInvocation &invocation,
                                                const ProcessLaunchContext &context) {
    if (entry.dbusActivatable) {
        return failure<ProcessLaunchPlan>(
            ErrorCode::unsupported,
            "A D-Bus-activatable desktop entry cannot use process launch planning.",
            "Use the separate D-Bus application activation path.");
    }
    if (entry.terminal && !context.terminal) {
        return failure<ProcessLaunchPlan>(
            ErrorCode::unsupported, "The desktop entry requires an unavailable terminal policy.",
            "Configure an argv-vector terminal policy or choose a non-terminal application.");
    }

    auto validatedInvocation = validateInvocation(invocation);
    if (!validatedInvocation) {
        return Result<ProcessLaunchPlan>::failure(validatedInvocation.error());
    }
    auto selectedWorkingDirectory = workingDirectory(entry, context);
    if (!selectedWorkingDirectory) {
        return Result<ProcessLaunchPlan>::failure(selectedWorkingDirectory.error());
    }
    auto environment = normalizedEnvironment(context.environment, selectedWorkingDirectory.value());
    if (!environment) {
        return Result<ProcessLaunchPlan>::failure(environment.error());
    }

    auto applicationExecutable =
        resolveDesktopExecutable(invocation.argv.front(), context.executableLookup);
    if (!applicationExecutable) {
        return Result<ProcessLaunchPlan>::failure(applicationExecutable.error());
    }

    std::vector<std::string> applicationArguments = invocation.argv;
    applicationArguments.front() = applicationExecutable.value().path().native();

    ProcessLaunchPlan plan;
    plan.workingDirectory = std::move(selectedWorkingDirectory).value();
    plan.environment = std::move(environment).value();
    if (!entry.terminal) {
        plan.executable = applicationExecutable.value().path();
        plan.argv = std::move(applicationArguments);
    } else {
        const auto &terminalPolicy = *context.terminal;
        if (terminalPolicy.argumentsBeforeCommand.size() >= maximumProcessLaunchArguments) {
            return failure<ProcessLaunchPlan>(ErrorCode::too_large,
                                              "The terminal launch prefix has too many arguments.",
                                              "Configure a smaller argv-vector terminal prefix.");
        }
        for (const auto &argument : terminalPolicy.argumentsBeforeCommand) {
            auto validated = validateArgument(argument);
            if (!validated) {
                return Result<ProcessLaunchPlan>::failure(validated.error());
            }
        }
        auto terminalExecutable =
            resolveDesktopExecutable(terminalPolicy.executable, context.executableLookup);
        if (!terminalExecutable) {
            return Result<ProcessLaunchPlan>::failure(terminalExecutable.error());
        }

        plan.executable = terminalExecutable.value().path();
        plan.argv.reserve(1U + terminalPolicy.argumentsBeforeCommand.size() +
                          applicationArguments.size());
        plan.argv.push_back(plan.executable.native());
        plan.argv.insert(plan.argv.end(), terminalPolicy.argumentsBeforeCommand.begin(),
                         terminalPolicy.argumentsBeforeCommand.end());
        plan.argv.insert(plan.argv.end(), applicationArguments.begin(), applicationArguments.end());
    }

    auto validatedEnvelope = validateFinalEnvelope(plan);
    if (!validatedEnvelope) {
        return Result<ProcessLaunchPlan>::failure(validatedEnvelope.error());
    }
    return Result<ProcessLaunchPlan>::success(std::move(plan));
}

} // namespace prismdrake::launcher
