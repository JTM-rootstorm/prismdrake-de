#include "DesktopExecutable.hpp"

#include <cerrno>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace prismdrake::launcher {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::Result;

enum class Availability : std::uint8_t {
    executable,
    missing,
    notRegularFile,
    notExecutable,
};

struct LookupResult final {
    Availability availability;
    std::optional<std::filesystem::path> resolvedPath;
};

struct ValidatedLookup final {
    std::filesystem::path executable;
    bool absoluteExecutable{false};
    std::vector<std::filesystem::path> searchDirectories;
};

struct ValidatedLookupContext final {
    std::vector<std::filesystem::path> searchDirectories;
};

template <typename Value>
[[nodiscard]] Result<Value> failure(ErrorCode code, std::string message, std::string recovery) {
    return Result<Value>::failure(Error{code, std::move(message), std::move(recovery)});
}

[[nodiscard]] bool isPrintableAscii(std::string_view value) noexcept {
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<std::filesystem::path>
validateLookupBase(const std::filesystem::path &lookupBase) {
    const auto native = lookupBase.native();
    if (lookupBase.empty() || !lookupBase.is_absolute() || native.find('\0') != std::string::npos ||
        native.size() > maximumDesktopExecutableCandidateBytes) {
        return failure<std::filesystem::path>(
            ErrorCode::invalid_argument,
            "The desktop executable lookup base is invalid or too large.",
            "Use an explicit bounded absolute lookup base directory.");
    }
    const auto normalized = lookupBase.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute()) {
        return failure<std::filesystem::path>(ErrorCode::invalid_argument,
                                              "The desktop executable lookup base is invalid.",
                                              "Use an explicit absolute lookup base directory.");
    }
    return Result<std::filesystem::path>::success(normalized);
}

[[nodiscard]] Result<std::vector<std::filesystem::path>>
parseSearchDirectories(std::string_view searchPath, const std::filesystem::path &lookupBase) {
    if (searchPath.size() > maximumDesktopExecutableSearchPathBytes) {
        return failure<std::vector<std::filesystem::path>>(
            ErrorCode::too_large, "The executable search PATH exceeds its byte limit.",
            "Use a smaller bounded PATH value.");
    }
    if (searchPath.find('\0') != std::string_view::npos) {
        return failure<std::vector<std::filesystem::path>>(
            ErrorCode::invalid_argument, "The executable search PATH contains a null byte.",
            "Use a bounded NUL-free PATH value.");
    }

    std::vector<std::filesystem::path> directories;
    std::size_t offset = 0U;
    for (;;) {
        if (directories.size() >= maximumDesktopExecutableSearchPathComponents) {
            return failure<std::vector<std::filesystem::path>>(
                ErrorCode::too_large, "The executable search PATH has too many components.",
                "Use fewer bounded PATH components.");
        }
        const auto separator = searchPath.find(':', offset);
        const auto end = separator == std::string_view::npos ? searchPath.size() : separator;
        const auto component = searchPath.substr(offset, end - offset);
        std::filesystem::path directory =
            component.empty() ? lookupBase : std::filesystem::path{component};
        if (!directory.is_absolute()) {
            directory = lookupBase / directory;
        }
        directory = directory.lexically_normal();
        const auto native = directory.native();
        if (directory.empty() || !directory.is_absolute() ||
            native.size() > maximumDesktopExecutableCandidateBytes) {
            return failure<std::vector<std::filesystem::path>>(
                ErrorCode::too_large,
                "An executable search PATH component produces an oversized candidate base.",
                "Use smaller absolute or lookup-base-relative PATH components.");
        }
        directories.push_back(std::move(directory));
        if (separator == std::string_view::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
}

[[nodiscard]] Result<ValidatedLookupContext>
validateLookupContext(const DesktopExecutableLookupContext &context) {
    auto base = validateLookupBase(context.lookupBase);
    if (!base) {
        return Result<ValidatedLookupContext>::failure(base.error());
    }
    auto directories = parseSearchDirectories(context.searchPath, base.value());
    if (!directories) {
        return Result<ValidatedLookupContext>::failure(directories.error());
    }
    return Result<ValidatedLookupContext>::success({std::move(directories).value()});
}

[[nodiscard]] Result<ValidatedLookup>
validateLookup(std::string_view executable, const DesktopExecutableLookupContext &context) {
    if (executable.empty() || executable.find('\0') != std::string_view::npos ||
        executable.find('=') != std::string_view::npos || !isPrintableAscii(executable)) {
        return failure<ValidatedLookup>(
            ErrorCode::invalid_argument, "The desktop executable name or path is invalid.",
            "Use a nonempty NUL-free printable-ASCII executable without an equals sign.");
    }

    auto validatedContext = validateLookupContext(context);
    if (!validatedContext) {
        return Result<ValidatedLookup>::failure(validatedContext.error());
    }

    std::filesystem::path executablePath{executable};
    if (executablePath.is_absolute()) {
        executablePath = executablePath.lexically_normal();
        if (executablePath.empty() || !executablePath.is_absolute() ||
            executablePath.native().size() > maximumDesktopExecutableCandidateBytes) {
            return failure<ValidatedLookup>(ErrorCode::too_large,
                                            "The absolute desktop executable path is too large.",
                                            "Use a smaller bounded absolute executable path.");
        }
        return Result<ValidatedLookup>::success(
            ValidatedLookup{std::move(executablePath), true, {}});
    }

    if (executable.find('/') != std::string_view::npos) {
        return failure<ValidatedLookup>(
            ErrorCode::invalid_argument, "A relative desktop executable contains a path separator.",
            "Use an absolute path or a bare executable name searched through PATH.");
    }
    if (executable.size() > maximumDesktopExecutableNameBytes) {
        return failure<ValidatedLookup>(ErrorCode::too_large,
                                        "The desktop executable name exceeds its byte limit.",
                                        "Use a smaller bounded executable name.");
    }

    for (const auto &directory : validatedContext.value().searchDirectories) {
        const auto candidate = (directory / executablePath).lexically_normal();
        if (!candidate.is_absolute() ||
            candidate.native().size() > maximumDesktopExecutableCandidateBytes) {
            return failure<ValidatedLookup>(
                ErrorCode::too_large,
                "Executable lookup would produce an oversized candidate path.",
                "Use smaller PATH components or a smaller executable name.");
        }
    }
    return Result<ValidatedLookup>::success(ValidatedLookup{
        std::move(executablePath), false, std::move(validatedContext).value().searchDirectories});
}

[[nodiscard]] Result<Availability> probeExecutable(const std::filesystem::path &candidate) {
    struct stat status = {};
    if (::stat(candidate.c_str(), &status) != 0) {
        const int errorNumber = errno;
        if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
            return Result<Availability>::success(Availability::missing);
        }
        if (errorNumber == EACCES || errorNumber == EPERM) {
            return Result<Availability>::success(Availability::notExecutable);
        }
        if (errorNumber == ENAMETOOLONG) {
            return failure<Availability>(ErrorCode::too_large,
                                         "An executable candidate exceeds a filesystem limit.",
                                         "Use smaller PATH components and executable names.");
        }
        return failure<Availability>(ErrorCode::io_error,
                                     "An executable candidate could not be inspected.",
                                     "Review filesystem availability and retry executable lookup.");
    }
    if (!S_ISREG(status.st_mode)) {
        return Result<Availability>::success(Availability::notRegularFile);
    }
    if (::access(candidate.c_str(), X_OK) == 0) {
        return Result<Availability>::success(Availability::executable);
    }
    const int errorNumber = errno;
    if (errorNumber == EACCES || errorNumber == EPERM) {
        return Result<Availability>::success(Availability::notExecutable);
    }
    if (errorNumber == ENOENT || errorNumber == ENOTDIR) {
        return Result<Availability>::success(Availability::missing);
    }
    return failure<Availability>(ErrorCode::io_error,
                                 "Executable permission could not be inspected.",
                                 "Review filesystem availability and retry executable lookup.");
}

[[nodiscard]] Result<LookupResult> performLookup(const ValidatedLookup &lookup) {
    if (lookup.absoluteExecutable) {
        auto availability = probeExecutable(lookup.executable);
        if (!availability) {
            return Result<LookupResult>::failure(availability.error());
        }
        return Result<LookupResult>::success(
            {availability.value(), availability.value() == Availability::executable
                                       ? std::optional{lookup.executable}
                                       : std::nullopt});
    }

    Availability firstUnavailable = Availability::missing;
    bool recordedUnavailable = false;
    std::optional<Error> firstError;
    for (const auto &directory : lookup.searchDirectories) {
        const auto candidate = (directory / lookup.executable).lexically_normal();
        auto availability = probeExecutable(candidate);
        if (!availability) {
            if (!firstError) {
                firstError = availability.error();
            }
            continue;
        }
        if (availability.value() == Availability::executable) {
            return Result<LookupResult>::success({Availability::executable, candidate});
        }
        if (availability.value() != Availability::missing && !recordedUnavailable) {
            firstUnavailable = availability.value();
            recordedUnavailable = true;
        }
    }
    if (firstError) {
        return Result<LookupResult>::failure(std::move(*firstError));
    }
    return Result<LookupResult>::success({firstUnavailable, std::nullopt});
}

[[nodiscard]] Result<LookupResult>
lookupDesktopExecutable(std::string_view executable,
                        const DesktopExecutableLookupContext &context) {
    auto validated = validateLookup(executable, context);
    if (!validated) {
        return Result<LookupResult>::failure(validated.error());
    }
    return performLookup(validated.value());
}

[[nodiscard]] DesktopTryExecEligibilityReason tryExecReason(Availability availability) noexcept {
    switch (availability) {
    case Availability::executable:
        return DesktopTryExecEligibilityReason::eligibleExecutable;
    case Availability::missing:
        return DesktopTryExecEligibilityReason::ineligibleMissing;
    case Availability::notRegularFile:
        return DesktopTryExecEligibilityReason::ineligibleNotRegularFile;
    case Availability::notExecutable:
        return DesktopTryExecEligibilityReason::ineligibleNotExecutable;
    }
    return DesktopTryExecEligibilityReason::ineligibleMissing;
}

} // namespace

Result<ResolvedDesktopExecutable>
resolveDesktopExecutable(std::string_view executable,
                         const DesktopExecutableLookupContext &context) {
    auto lookup = lookupDesktopExecutable(executable, context);
    if (!lookup) {
        return Result<ResolvedDesktopExecutable>::failure(lookup.error());
    }
    switch (lookup.value().availability) {
    case Availability::executable:
        return Result<ResolvedDesktopExecutable>::success(
            ResolvedDesktopExecutable{std::move(*lookup.value().resolvedPath)});
    case Availability::missing:
        return failure<ResolvedDesktopExecutable>(
            ErrorCode::not_found, "The desktop executable could not be found.",
            "Install the application or correct its Exec value and PATH.");
    case Availability::notRegularFile:
        return failure<ResolvedDesktopExecutable>(
            ErrorCode::unsupported, "The desktop executable candidate is not a regular file.",
            "Select a regular executable file rather than a directory or special file.");
    case Availability::notExecutable:
        return failure<ResolvedDesktopExecutable>(
            ErrorCode::permission_denied, "The desktop executable candidate is not executable.",
            "Grant execute permission or select another executable.");
    }
    return failure<ResolvedDesktopExecutable>(ErrorCode::io_error,
                                              "Desktop executable lookup reached an invalid state.",
                                              "Retry lookup with validated inputs.");
}

Result<DesktopTryExecEligibility>
evaluateDesktopTryExec(const std::optional<std::string> &tryExec,
                       const DesktopExecutableLookupContext &context) {
    if (!tryExec) {
        auto validatedContext = validateLookupContext(context);
        if (!validatedContext) {
            return Result<DesktopTryExecEligibility>::failure(validatedContext.error());
        }
        return Result<DesktopTryExecEligibility>::success(
            {DesktopTryExecEligibilityReason::eligibleWithoutTryExec, std::nullopt});
    }
    auto lookup = lookupDesktopExecutable(*tryExec, context);
    if (!lookup) {
        return Result<DesktopTryExecEligibility>::failure(lookup.error());
    }
    return Result<DesktopTryExecEligibility>::success(
        {tryExecReason(lookup.value().availability), std::move(lookup.value().resolvedPath)});
}

} // namespace prismdrake::launcher
