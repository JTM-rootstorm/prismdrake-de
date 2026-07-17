#pragma once

#include "Configuration.hpp"
#include "Result.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace prismdrake::config {

inline constexpr std::size_t maximumConfigurationBytes = std::size_t{1024} * 1024U;
inline constexpr std::size_t maximumConfigurationStringCodePoints = 128U;
inline constexpr std::size_t maximumConfigurationStringBytes =
    maximumConfigurationStringCodePoints * 4U;
inline constexpr std::size_t maximumConfigurationArrayItems = 64U;
inline constexpr std::size_t maximumConfigurationTableEntries = 32U;
inline constexpr std::size_t maximumConfigurationNodes = 256U;
inline constexpr std::size_t maximumConfigurationNesting = 2U;

enum class DeveloperSettingsPolicy : std::uint8_t {
    production,
    developer,
};

struct ConfigurationParseOptions final {
    DeveloperSettingsPolicy developerSettingsPolicy = DeveloperSettingsPolicy::production;
};

/// Opaque syntax-valid TOML tree used to keep syntax and semantic validation separable.
class ParsedConfiguration final {
  public:
    ParsedConfiguration(ParsedConfiguration &&) noexcept;
    ParsedConfiguration &operator=(ParsedConfiguration &&) noexcept;
    ~ParsedConfiguration();

    ParsedConfiguration(const ParsedConfiguration &) = delete;
    ParsedConfiguration &operator=(const ParsedConfiguration &) = delete;

  private:
    struct Impl;

    explicit ParsedConfiguration(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;

    friend foundation::Result<ParsedConfiguration> parseConfigurationSyntax(std::string_view input);
    friend foundation::Result<Configuration>
    validateConfiguration(const ParsedConfiguration &document, ConfigurationParseOptions options);
};

/// Parses bounded TOML syntax. Errors contain coordinates but never input content.
[[nodiscard]] foundation::Result<ParsedConfiguration>
parseConfigurationSyntax(std::string_view input);

/// Validates a syntax-valid document against the complete version-1 semantic contract.
[[nodiscard]] foundation::Result<Configuration>
validateConfiguration(const ParsedConfiguration &document, ConfigurationParseOptions options = {});

/// Fuzz-friendly bounded convenience entry point for syntax and semantic validation.
[[nodiscard]] foundation::Result<Configuration>
parseConfigurationToml(std::string_view input, ConfigurationParseOptions options = {});

} // namespace prismdrake::config
