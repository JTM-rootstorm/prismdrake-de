#include "ServiceOptions.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace prismdrake::settingsd {
namespace {

template <typename T> [[nodiscard]] T requireOptional(std::optional<T> value) {
    if (!value) {
        throw std::logic_error("required test optional is empty");
    }
    return std::move(value).value();
}

class EnvironmentValue final {
  public:
    EnvironmentValue(const char *name, const std::filesystem::path &value) : name_(name) {
        if (const char *current = std::getenv(name); current != nullptr) {
            previous_ = current;
        }
        EXPECT_EQ(::setenv(name, value.c_str(), 1), 0);
    }

    ~EnvironmentValue() {
        if (previous_) {
            (void)::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

TEST(ServiceOptionsTest, HelpAndVersionDoNotRequireRuntimePaths) {
    const std::array helpArguments{std::string_view{"--help"}};
    const auto help = parseServiceOptions(helpArguments, {});
    ASSERT_TRUE(help);
    EXPECT_EQ(help.value().action, ServiceAction::show_help);
    EXPECT_FALSE(help.value().runtime);
    EXPECT_NE(serviceHelpText().find("--foreground"), std::string::npos);
    EXPECT_NE(serviceHelpText().find("--safe-mode"), std::string::npos);

    const std::array versionArguments{std::string_view{"--version"}};
    const auto version = parseServiceOptions(versionArguments, {});
    ASSERT_TRUE(version);
    EXPECT_EQ(version.value().action, ServiceAction::show_version);
    EXPECT_FALSE(version.value().runtime);
    EXPECT_NE(serviceVersionText().find("prismdrake-settingsd"), std::string::npos);
}

TEST(ServiceOptionsTest, RejectsUnknownDuplicateAndConflictingArgumentsWithoutEchoingThem) {
    constexpr std::string_view sentinel = "--secret-path=/private/sentinel";
    const std::array unknown{sentinel};
    const auto unsupported = parseServiceOptions(unknown, {});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().message.find(sentinel), std::string::npos);

    const std::array duplicate{std::string_view{"--foreground"}, std::string_view{"--foreground"}};
    EXPECT_FALSE(parseServiceOptions(duplicate, {}));

    const std::array duplicateSafeMode{std::string_view{"--safe-mode"},
                                       std::string_view{"--safe-mode"}};
    EXPECT_FALSE(parseServiceOptions(duplicateSafeMode, {}));

    const std::array conflicting{std::string_view{"--help"}, std::string_view{"--version"}};
    EXPECT_FALSE(parseServiceOptions(conflicting, {}));

    const std::array safeModeHelp{std::string_view{"--safe-mode"}, std::string_view{"--help"}};
    EXPECT_FALSE(parseServiceOptions(safeModeHelp, {}));
}

TEST(ServiceOptionsTest, ResolvesFixedPackagedAndXdgLocationsForRun) {
    const auto root = std::filesystem::temp_directory_path() / "prismdrake-service-options-test";
    EnvironmentValue home{"HOME", root / "home"};
    EnvironmentValue config{"XDG_CONFIG_HOME", root / "config"};
    EnvironmentValue data{"XDG_DATA_HOME", root / "data"};
    EnvironmentValue state{"XDG_STATE_HOME", root / "state"};
    EnvironmentValue cache{"XDG_CACHE_HOME", root / "cache"};
    EnvironmentValue runtime{"XDG_RUNTIME_DIR", root / "runtime"};

    const std::array arguments{std::string_view{"--foreground"}};
    const ServicePathDefaults defaults{"/usr/share/prismdrake/defaults/config.toml",
                                       "/usr/share/prismdrake/themes"};
    auto options = parseServiceOptions(arguments, defaults);

    ASSERT_TRUE(options);
    ASSERT_TRUE(options.value().runtime);
    EXPECT_TRUE(options.value().foreground);
    const auto runtimeOptions = requireOptional(std::move(options.value().runtime));
    EXPECT_EQ(runtimeOptions.settingsEngine.configurationLocations.packagedDefault,
              defaults.packagedConfiguration);
    EXPECT_EQ(runtimeOptions.settingsEngine.configurationLocations.user,
              root / "config/prismdrake/config.toml");
    EXPECT_EQ(runtimeOptions.settingsEngine.themeDirectory, defaults.themeDirectory);
    EXPECT_EQ(runtimeOptions.settingsEngine.mode, settings::SettingsEngineMode::normal);
}

TEST(ServiceOptionsTest, EnablesNonpersistentSafeModeForTheSettingsEngine) {
    const auto root = std::filesystem::temp_directory_path() / "prismdrake-safe-options-test";
    EnvironmentValue home{"HOME", root / "home"};
    EnvironmentValue config{"XDG_CONFIG_HOME", root / "config"};
    EnvironmentValue data{"XDG_DATA_HOME", root / "data"};
    EnvironmentValue state{"XDG_STATE_HOME", root / "state"};
    EnvironmentValue cache{"XDG_CACHE_HOME", root / "cache"};
    EnvironmentValue runtime{"XDG_RUNTIME_DIR", root / "runtime"};

    const std::array arguments{std::string_view{"--safe-mode"}};
    const ServicePathDefaults defaults{"/usr/share/prismdrake/defaults/config.toml",
                                       "/usr/share/prismdrake/themes"};
    auto options = parseServiceOptions(arguments, defaults);

    ASSERT_TRUE(options);
    ASSERT_TRUE(options.value().runtime);
    const auto runtimeOptions = requireOptional(std::move(options.value().runtime));
    EXPECT_EQ(runtimeOptions.settingsEngine.mode,
              settings::SettingsEngineMode::development_safe_mode);
}

} // namespace
} // namespace prismdrake::settingsd
