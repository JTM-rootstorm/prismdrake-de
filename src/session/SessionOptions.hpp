#pragma once

#include "Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace prismdrake::session {

inline constexpr std::size_t maximumSessionExecutablePathBytes = 4095U;

/// Process actions accepted by the development session command-line interface.
enum class SessionAction : std::uint8_t {
    run,
    show_help,
    show_version,
};

/// Build- or packaging-owned component locations used when no local override is
/// supplied. Empty paths deliberately mean that the corresponding default is
/// unavailable; executables are never resolved through PATH.
struct SessionPathDefaults final {
    std::filesystem::path settingsdExecutable;
    std::filesystem::path shellExecutable;
};

/// Validated absolute component paths required to start a development session.
struct SessionRuntimeOptions final {
    std::filesystem::path settingsdExecutable;
    std::filesystem::path shellExecutable;
};

/// Parsed process options. Runtime paths are populated only for the run action.
struct SessionOptions final {
    SessionAction action = SessionAction::run;
    std::optional<SessionRuntimeOptions> runtime;
};

/// Parses arguments excluding argv[0]. Runtime component locations must be
/// supplied exactly once or provided by fixed defaults. Selected paths must be
/// absolute regular executables and are never included in returned errors.
[[nodiscard]] foundation::Result<SessionOptions>
parseSessionOptions(std::span<const std::string_view> arguments,
                    const SessionPathDefaults &pathDefaults = {});

/// Returns stable, path-free command-line help text.
[[nodiscard]] std::string sessionHelpText();

/// Returns the tracked product version in a stable, path-free format.
[[nodiscard]] std::string sessionVersionText();

} // namespace prismdrake::session
