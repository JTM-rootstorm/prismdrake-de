#include "ConfigurationLoader.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace prismdrake::config {
namespace {

using foundation::ErrorCode;

class ConfigurationStorageFixture final {
  public:
    ConfigurationStorageFixture() {
        std::string pathTemplate = "/tmp/prismdrake-config-loader-tests.XXXXXX";
        char *created = ::mkdtemp(pathTemplate.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
        std::filesystem::create_directories(root_ / "config");
        std::filesystem::create_directories(root_ / "state");
        std::filesystem::create_directories(root_ / "packaged");
    }

    ~ConfigurationStorageFixture() { std::filesystem::remove_all(root_); }

    [[nodiscard]] ConfigurationLocations locations() const {
        return {
            root_ / "config/config.toml",
            root_ / "state/last-known-valid-config.toml",
            root_ / "packaged/config.toml",
        };
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] std::string readFixture(std::string_view relativePath) {
    const std::filesystem::path path = std::filesystem::path{PRISMDRAKE_SOURCE_DIR} / relativePath;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open committed configuration fixture");
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeDocument(const std::filesystem::path &path, std::string_view document) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output);
    output.write(document.data(), static_cast<std::streamsize>(document.size()));
    ASSERT_TRUE(output);
}

[[nodiscard]] std::string readDocument(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(ConfigurationLoaderTest, SelectsStableLocationsUnderResolvedXdgDirectories) {
    const foundation::XdgPaths paths{
        "/home/test/.config/prismdrake",      "/home/test/.local/share/prismdrake",
        "/home/test/.local/state/prismdrake", "/home/test/.cache/prismdrake",
        "/run/user/1000/prismdrake",
    };

    const auto locations =
        configurationLocations(paths, "/usr/share/prismdrake/defaults/config.toml");

    EXPECT_EQ(locations.user, "/home/test/.config/prismdrake/config.toml");
    EXPECT_EQ(locations.lastKnownValid,
              "/home/test/.local/state/prismdrake/last-known-valid-config.toml");
    EXPECT_EQ(locations.packagedDefault, "/usr/share/prismdrake/defaults/config.toml");
}

TEST(ConfigurationLoaderTest, PrefersACompleteValidUserConfiguration) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, readFixture("examples/config/forge.toml"));
    writeDocument(locations.lastKnownValid, readFixture("examples/config/lustre.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadStartupConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().candidate.source, ConfigurationSource::user);
    EXPECT_EQ(result.value().candidate.configuration.profile, Profile::forge);
    EXPECT_TRUE(result.value().issues.empty());
}

TEST(ConfigurationLoaderTest, MissingUserConfigurationSelectsPackagedDefaultNotStaleLkv) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.lastKnownValid, readFixture("examples/config/forge.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadStartupConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().candidate.source, ConfigurationSource::packaged_default);
    EXPECT_EQ(result.value().candidate.configuration.profile, Profile::lustre);
    EXPECT_TRUE(result.value().issues.empty());
}

TEST(ConfigurationLoaderTest, InvalidUserConfigurationRecoversThroughLkv) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, "schema_version = 2\n");
    writeDocument(locations.lastKnownValid, readFixture("examples/config/forge.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadStartupConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().candidate.source, ConfigurationSource::last_known_valid);
    EXPECT_EQ(result.value().candidate.configuration.profile, Profile::forge);
    ASSERT_EQ(result.value().issues.size(), 1U);
    EXPECT_EQ(result.value().issues.front().source, ConfigurationSource::user);
    EXPECT_EQ(result.value().issues.front().error.code, ErrorCode::unsupported);
}

TEST(ConfigurationLoaderTest, InvalidUserAndLkvRecoverThroughPackagedDefault) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, "not valid TOML = [");
    writeDocument(locations.lastKnownValid, "schema_version = 9\n");
    writeDocument(locations.packagedDefault, readFixture("examples/config/forge.toml"));

    const auto result = loadStartupConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().candidate.source, ConfigurationSource::packaged_default);
    EXPECT_EQ(result.value().candidate.configuration.profile, Profile::forge);
    ASSERT_EQ(result.value().issues.size(), 2U);
    EXPECT_EQ(result.value().issues[0].source, ConfigurationSource::user);
    EXPECT_EQ(result.value().issues[1].source, ConfigurationSource::last_known_valid);
}

TEST(ConfigurationLoaderTest, FailsWhenNoCompleteValidSourceExists) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, "schema_version = 2\n");
    writeDocument(locations.lastKnownValid, "schema_version = 2\n");
    writeDocument(locations.packagedDefault, "schema_version = 2\n");

    const auto result = loadStartupConfiguration(locations);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
    EXPECT_NE(result.error().message.find("$.schema_version"), std::string::npos);
    EXPECT_EQ(result.error().message.find(locations.user.string()), std::string::npos);
}

TEST(ConfigurationLoaderTest, InvalidReloadFailsSoCallerCanRetainCurrentGeneration) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, "schema_version = 2\n");
    writeDocument(locations.lastKnownValid, readFixture("examples/config/forge.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadReloadConfiguration(locations);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
}

TEST(ConfigurationLoaderTest, MissingUserReloadExplicitlySelectsPackagedDefault) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.lastKnownValid, readFixture("examples/config/forge.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadReloadConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().source, ConfigurationSource::packaged_default);
    EXPECT_EQ(result.value().configuration.profile, Profile::lustre);
}

TEST(ConfigurationLoaderTest, PackagedRecoveryIgnoresUserAndLastKnownValidSources) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, readFixture("examples/config/forge.toml"));
    writeDocument(locations.lastKnownValid, readFixture("examples/config/forge.toml"));
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadPackagedConfiguration(locations);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().source, ConfigurationSource::packaged_default);
    EXPECT_EQ(result.value().configuration.profile, Profile::lustre);
}

TEST(ConfigurationLoaderTest, InvalidCandidateNeverReplacesUserConfiguration) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    const std::string previous = readFixture("examples/config/forge.toml");
    writeDocument(locations.user, previous);

    const auto result = validateAndWriteUserConfiguration(locations, "schema_version = 2\n");

    ASSERT_FALSE(result);
    EXPECT_EQ(readDocument(locations.user), previous);
}

TEST(ConfigurationLoaderTest, ValidCandidateIsWrittenAsOneCompleteDocument) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    writeDocument(locations.user, readFixture("examples/config/forge.toml"));
    const std::string replacement = readFixture("examples/config/accessible.toml");

    const auto result = validateAndWriteUserConfiguration(locations, replacement);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().profile, Profile::lustre);
    EXPECT_EQ(readDocument(locations.user), replacement);
}

TEST(ConfigurationLoaderTest, PromotesOnlyAValidatedUserCandidate) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    const std::string userDocument = readFixture("examples/config/forge.toml");
    writeDocument(locations.user, userDocument);
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    auto user = loadReloadConfiguration(locations);
    ASSERT_TRUE(user);
    ASSERT_TRUE(promoteLastKnownValidConfiguration(locations, user.value()));
    EXPECT_EQ(readDocument(locations.lastKnownValid), userDocument);

    std::filesystem::remove(locations.user);
    auto packaged = loadReloadConfiguration(locations);
    ASSERT_TRUE(packaged);
    const auto rejected = promoteLastKnownValidConfiguration(locations, packaged.value());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(readDocument(locations.lastKnownValid), userDocument);
}

TEST(ConfigurationLoaderTest, RevalidatesCandidateBytesBeforeLkvPromotion) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    const std::string previous = readFixture("examples/config/lustre.toml");
    writeDocument(locations.user, readFixture("examples/config/forge.toml"));
    writeDocument(locations.lastKnownValid, previous);

    auto candidate = loadReloadConfiguration(locations);
    ASSERT_TRUE(candidate);
    candidate.value().originalDocument = "schema_version = 2\n";

    const auto result = promoteLastKnownValidConfiguration(locations, candidate.value());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
    EXPECT_EQ(readDocument(locations.lastKnownValid), previous);
}

TEST(ConfigurationLoaderTest, RejectsValidCandidateBytesThatDoNotMatchPublishedValues) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    const std::string previous = readFixture("examples/config/accessible.toml");
    writeDocument(locations.user, readFixture("examples/config/forge.toml"));
    writeDocument(locations.lastKnownValid, previous);

    auto candidate = loadReloadConfiguration(locations);
    ASSERT_TRUE(candidate);
    candidate.value().originalDocument = readFixture("examples/config/lustre.toml");

    const auto result = promoteLastKnownValidConfiguration(locations, candidate.value());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::validation_error);
    EXPECT_EQ(readDocument(locations.lastKnownValid), previous);
}

TEST(ConfigurationLoaderTest, RecoveryIssuesNeverEchoPrivatePathsKeysOrValues) {
    ConfigurationStorageFixture storage;
    const auto locations = storage.locations();
    const auto invalid =
        readFixture("examples/config/lustre.toml") + "\nprivate_key = \"super-secret-value\"\n";
    writeDocument(locations.user, invalid);
    writeDocument(locations.packagedDefault, readFixture("data/defaults/config.toml"));

    const auto result = loadStartupConfiguration(locations);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().issues.size(), 1U);
    const auto &error = result.value().issues.front().error;
    EXPECT_EQ(error.message.find(locations.user.string()), std::string::npos);
    EXPECT_EQ(error.message.find("private_key"), std::string::npos);
    EXPECT_EQ(error.message.find("super-secret-value"), std::string::npos);
    EXPECT_EQ(error.recovery.find("super-secret-value"), std::string::npos);
}

} // namespace
} // namespace prismdrake::config
