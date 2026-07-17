#include "XdgPaths.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace prismdrake::foundation {
namespace {

XdgEnvironment environmentWithRuntime() {
    XdgEnvironment environment;
    environment.home = "/home/tester";
    environment.runtimeDirectory = "/run/user/1000";
    return environment;
}

class RuntimeDirectoryFixture {
  public:
    RuntimeDirectoryFixture() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "prismdrake-xdg-XXXXXX").string();
        char *createdDirectory = ::mkdtemp(pattern.data());
        if (createdDirectory == nullptr) {
            throw std::runtime_error{"Could not create runtime-directory test fixture"};
        }

        root = createdDirectory;
        base = root / "runtime";
        prismdrake = base / "prismdrake";
        std::filesystem::create_directories(prismdrake);
        setPrivate(base);
        setPrivate(prismdrake);
    }

    ~RuntimeDirectoryFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    RuntimeDirectoryFixture(const RuntimeDirectoryFixture &) = delete;
    RuntimeDirectoryFixture &operator=(const RuntimeDirectoryFixture &) = delete;

    static void setPrivate(const std::filesystem::path &directory) {
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    std::filesystem::path root;
    std::filesystem::path base;
    std::filesystem::path prismdrake;
};

TEST(XdgPathsTest, UsesDocumentedHomeDefaultsWhenXdgHomesAreAbsent) {
    const auto result = resolveXdgPaths(environmentWithRuntime());

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().configDirectory, "/home/tester/.config/prismdrake");
    EXPECT_EQ(result.value().dataDirectory, "/home/tester/.local/share/prismdrake");
    EXPECT_EQ(result.value().stateDirectory, "/home/tester/.local/state/prismdrake");
    EXPECT_EQ(result.value().cacheDirectory, "/home/tester/.cache/prismdrake");
    EXPECT_EQ(result.value().runtimeDirectory, "/run/user/1000/prismdrake");
}

TEST(XdgPathsTest, HonorsAbsoluteXdgOverridesWithoutHome) {
    XdgEnvironment environment;
    environment.configHome = "/vol/config";
    environment.dataHome = "/vol/data";
    environment.stateHome = "/vol/state";
    environment.cacheHome = "/vol/cache";
    environment.runtimeDirectory = "/vol/runtime";

    const auto result = resolveXdgPaths(environment);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().configDirectory, "/vol/config/prismdrake");
    EXPECT_EQ(result.value().dataDirectory, "/vol/data/prismdrake");
    EXPECT_EQ(result.value().stateDirectory, "/vol/state/prismdrake");
    EXPECT_EQ(result.value().cacheDirectory, "/vol/cache/prismdrake");
    EXPECT_EQ(result.value().runtimeDirectory, "/vol/runtime/prismdrake");
}

TEST(XdgPathsTest, FallsBackForRelativeOrEmptyNonRuntimeValues) {
    XdgEnvironment environment = environmentWithRuntime();
    environment.configHome = "relative/config";
    environment.dataHome = "";
    environment.stateHome = ".state";
    environment.cacheHome = "cache";

    const auto result = resolveXdgPaths(environment);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().configDirectory, "/home/tester/.config/prismdrake");
    EXPECT_EQ(result.value().dataDirectory, "/home/tester/.local/share/prismdrake");
    EXPECT_EQ(result.value().stateDirectory, "/home/tester/.local/state/prismdrake");
    EXPECT_EQ(result.value().cacheDirectory, "/home/tester/.cache/prismdrake");
}

TEST(XdgPathsTest, RequiresHomeWhenAnyNonRuntimeValueNeedsItsDefault) {
    XdgEnvironment environment;
    environment.configHome = "/vol/config";
    environment.dataHome = "/vol/data";
    environment.stateHome = "/vol/state";
    environment.runtimeDirectory = "/run/user/1000";

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message,
              "HOME must be set to a valid absolute path when an XDG default is required.");
}

TEST(XdgPathsTest, RejectsRelativeHomeWhenFallbackIsNeeded) {
    XdgEnvironment environment = environmentWithRuntime();
    environment.home = "users/tester";

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message, "HOME must be absolute when an XDG default is required.");
}

TEST(XdgPathsTest, RejectsAbsentRuntimeDirectory) {
    XdgEnvironment environment = environmentWithRuntime();
    environment.runtimeDirectory.reset();

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message, "XDG_RUNTIME_DIR is required for Prismdrake runtime state.");
}

TEST(XdgPathsTest, RejectsRelativeRuntimeDirectoryWithoutUsingHomeFallback) {
    XdgEnvironment environment = environmentWithRuntime();
    environment.runtimeDirectory = "run/user/1000";

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message, "XDG_RUNTIME_DIR must be absolute.");
}

TEST(XdgPathsTest, ErrorDetailsDoNotRepeatEnvironmentContent) {
    constexpr auto privateValue = "private-user-content/runtime-location";
    XdgEnvironment environment = environmentWithRuntime();
    environment.runtimeDirectory = privateValue;

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message.find(privateValue), std::string::npos);
    EXPECT_EQ(result.error().recovery.find(privateValue), std::string::npos);
}

TEST(XdgPathsTest, RejectsEmbeddedNullWithoutDisclosingTheValue) {
    const std::string privateValue{"/run/user/1000\0private", 22};
    XdgEnvironment environment = environmentWithRuntime();
    environment.runtimeDirectory = privateValue;

    const auto result = resolveXdgPaths(environment);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message.find("private"), std::string::npos);
    EXPECT_EQ(result.error().recovery.find("private"), std::string::npos);
}

TEST(XdgPathsTest, AcceptsPrivateRuntimeBoundaryOwnedByExpectedUser) {
    RuntimeDirectoryFixture fixture;

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    EXPECT_TRUE(result.hasValue());
}

TEST(XdgPathsTest, RejectsMissingRuntimeSubdirectory) {
    RuntimeDirectoryFixture fixture;
    std::filesystem::remove(fixture.prismdrake);

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::not_found);
    EXPECT_EQ(result.error().message, "Prismdrake runtime subdirectory does not exist.");
}

TEST(XdgPathsTest, RejectsRuntimeSubdirectoryWithNonPrivateMode) {
    RuntimeDirectoryFixture fixture;
    std::filesystem::permissions(
        fixture.prismdrake, std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace);

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::permission_denied);
    EXPECT_EQ(result.error().message,
              "Prismdrake runtime subdirectory must have private mode 0700.");
}

TEST(XdgPathsTest, RejectsRuntimeBoundaryOwnedByUnexpectedUser) {
    RuntimeDirectoryFixture fixture;
    const auto unexpectedUser = currentProcessUserId() + 1U;

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, unexpectedUser);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::permission_denied);
    EXPECT_EQ(result.error().message, "XDG runtime base directory is owned by an unexpected user.");
}

TEST(XdgPathsTest, RejectsRegularFileAsRuntimeSubdirectory) {
    RuntimeDirectoryFixture fixture;
    std::filesystem::remove(fixture.prismdrake);
    std::ofstream{fixture.prismdrake} << "not a directory";

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message, "Prismdrake runtime subdirectory is not a directory.");
}

TEST(XdgPathsTest, RejectsSymbolicLinkAsRuntimeSubdirectory) {
    RuntimeDirectoryFixture fixture;
    const auto linkTarget = fixture.root / "link-target";
    std::filesystem::create_directory(linkTarget);
    RuntimeDirectoryFixture::setPrivate(linkTarget);
    std::filesystem::remove(fixture.prismdrake);
    std::filesystem::create_directory_symlink(linkTarget, fixture.prismdrake);

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, ErrorCode::invalid_environment);
    EXPECT_EQ(result.error().message,
              "Prismdrake runtime subdirectory must not be a symbolic link.");
}

TEST(XdgPathsTest, RuntimeValidationErrorsDoNotDiscloseFilesystemPaths) {
    RuntimeDirectoryFixture fixture;
    std::filesystem::remove(fixture.prismdrake);

    const auto result =
        validateRuntimeDirectoryBoundary(fixture.base, fixture.prismdrake, currentProcessUserId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message.find(fixture.root.string()), std::string::npos);
    EXPECT_EQ(result.error().recovery.find(fixture.root.string()), std::string::npos);
}

} // namespace
} // namespace prismdrake::foundation
