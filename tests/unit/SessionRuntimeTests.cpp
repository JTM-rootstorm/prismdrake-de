#include "SessionRuntime.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace prismdrake::session {
namespace {

using foundation::ErrorCode;

class RuntimeFixture final {
  public:
    explicit RuntimeFixture(bool createPrismdrakeRoot = true) {
        std::string pattern = "/tmp/prismdrake-session-runtime-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create a session runtime fixture."};
        }
        root_ = created;
        base_ = root_ / "runtime-private-sentinel";
        prismdrake_ = base_ / "prismdrake";
        std::filesystem::create_directories(base_);
        setPrivate(base_);
        if (createPrismdrakeRoot) {
            std::filesystem::create_directory(prismdrake_);
            setPrivate(prismdrake_);
        }
    }

    ~RuntimeFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    RuntimeFixture(const RuntimeFixture &) = delete;
    RuntimeFixture &operator=(const RuntimeFixture &) = delete;

    [[nodiscard]] foundation::XdgPaths paths() const { return {{}, {}, {}, {}, prismdrake_}; }

    [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }
    [[nodiscard]] const std::filesystem::path &base() const noexcept { return base_; }
    [[nodiscard]] const std::filesystem::path &prismdrake() const noexcept { return prismdrake_; }

    static void setPrivate(const std::filesystem::path &path) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path base_;
    std::filesystem::path prismdrake_;
};

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output << contents;
    ASSERT_TRUE(output);
}

[[nodiscard]] mode_t permissionsOf(const std::filesystem::path &path) {
    struct stat metadata = {};
    EXPECT_EQ(::lstat(path.c_str(), &metadata), 0);
    return metadata.st_mode & 0777U;
}

TEST(SessionRuntimeTest, CreatesPrivateInstanceMarkersAndRemovesOnlyItsOwnDirectory) {
    RuntimeFixture fixture;
    const auto siblingFile = fixture.prismdrake() / "preexisting-sibling";
    const auto siblingDirectory = fixture.prismdrake() / "preexisting-directory";
    writeFile(siblingFile, "preserve");
    ASSERT_TRUE(std::filesystem::create_directory(siblingDirectory));

    auto runtime = SessionRuntime::prepare(fixture.paths());

    ASSERT_TRUE(runtime);
    const auto instance = runtime.value().instanceDirectory();
    EXPECT_EQ(instance.parent_path(), fixture.prismdrake());
    EXPECT_EQ(instance.filename().string().find("session-" + std::to_string(::getpid()) + "-"), 0U);
    EXPECT_EQ(permissionsOf(instance), 0700U);
    ASSERT_TRUE(runtime.value().markReady());
    ASSERT_TRUE(runtime.value().markReady());
    ASSERT_TRUE(runtime.value().markSafeMode());
    ASSERT_TRUE(runtime.value().markSafeMode());
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().readyMarkerPath()));
    EXPECT_TRUE(std::filesystem::is_regular_file(runtime.value().safeModeMarkerPath()));
    EXPECT_EQ(permissionsOf(runtime.value().readyMarkerPath()), 0600U);
    EXPECT_EQ(permissionsOf(runtime.value().safeModeMarkerPath()), 0600U);

    ASSERT_TRUE(runtime.value().cleanup());
    EXPECT_FALSE(std::filesystem::exists(instance));
    EXPECT_TRUE(std::filesystem::is_regular_file(siblingFile));
    EXPECT_TRUE(std::filesystem::is_directory(siblingDirectory));
    EXPECT_TRUE(runtime.value().cleanup());
    EXPECT_FALSE(runtime.value().markReady());
    EXPECT_TRUE(std::filesystem::is_directory(fixture.prismdrake()));
}

TEST(SessionRuntimeTest, CreatesAndPreservesTheSharedPrismdrakeRoot) {
    RuntimeFixture fixture{false};
    const auto baseSibling = fixture.base() / "preexisting-base-sibling";
    writeFile(baseSibling, "preserve");
    auto runtime = SessionRuntime::prepare(fixture.paths());

    ASSERT_TRUE(runtime);
    EXPECT_TRUE(std::filesystem::is_directory(fixture.prismdrake()));
    EXPECT_EQ(permissionsOf(fixture.prismdrake()), 0700U);
    const auto sharedSibling = fixture.prismdrake() / "shared-sibling";
    writeFile(sharedSibling, "preserve");
    ASSERT_TRUE(runtime.value().cleanup());
    EXPECT_TRUE(std::filesystem::is_directory(fixture.prismdrake()));
    EXPECT_TRUE(std::filesystem::is_regular_file(sharedSibling));
    EXPECT_TRUE(std::filesystem::is_regular_file(baseSibling));
}

TEST(SessionRuntimeTest, AllocatesDistinctCounterInstancesWithoutCleaningSiblings) {
    RuntimeFixture fixture;
    auto first = SessionRuntime::prepare(fixture.paths());
    auto second = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value().instanceDirectory(), second.value().instanceDirectory());

    const auto secondInstance = second.value().instanceDirectory();
    ASSERT_TRUE(first.value().cleanup());
    EXPECT_TRUE(std::filesystem::is_directory(secondInstance));
    ASSERT_TRUE(second.value().cleanup());
}

TEST(SessionRuntimeTest, RejectsChangedBoundaryWithoutDisclosingItsPath) {
    RuntimeFixture fixture;
    std::filesystem::permissions(fixture.prismdrake(),
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::replace);

    const auto runtime = SessionRuntime::prepare(fixture.paths());

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, ErrorCode::permission_denied);
    EXPECT_EQ(runtime.error().message.find("runtime-private-sentinel"), std::string::npos);
    EXPECT_EQ(runtime.error().recovery.find("runtime-private-sentinel"), std::string::npos);
}

TEST(SessionRuntimeTest, RejectsANonPrivateXdgRuntimeBase) {
    RuntimeFixture fixture;
    std::filesystem::permissions(
        fixture.base(), std::filesystem::perms::owner_all | std::filesystem::perms::group_exec,
        std::filesystem::perm_options::replace);

    const auto runtime = SessionRuntime::prepare(fixture.paths());

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, ErrorCode::permission_denied);
    EXPECT_TRUE(std::filesystem::is_directory(fixture.prismdrake()));
}

TEST(SessionRuntimeTest, RejectsSymbolicLinkBoundaryWithoutTouchingItsTarget) {
    RuntimeFixture fixture;
    const auto target = fixture.root() / "alternate";
    std::filesystem::create_directory(target);
    RuntimeFixture::setPrivate(target);
    std::filesystem::remove(fixture.prismdrake());
    ASSERT_EQ(::symlink(target.c_str(), fixture.prismdrake().c_str()), 0);

    const auto runtime = SessionRuntime::prepare(fixture.paths());

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, ErrorCode::invalid_environment);
    EXPECT_TRUE(std::filesystem::is_symlink(fixture.prismdrake()));
    EXPECT_TRUE(std::filesystem::is_directory(target));
}

TEST(SessionRuntimeTest, RejectsSymbolicLinksInTheRuntimeAncestorChain) {
    RuntimeFixture fixture;
    const auto actual = fixture.root() / "actual";
    const auto nested = actual / "nested";
    const auto prismdrake = nested / "prismdrake";
    std::filesystem::create_directories(prismdrake);
    RuntimeFixture::setPrivate(nested);
    RuntimeFixture::setPrivate(prismdrake);
    const auto linkedAncestor = fixture.root() / "linked-ancestor";
    ASSERT_EQ(::symlink(actual.c_str(), linkedAncestor.c_str()), 0);
    auto paths = fixture.paths();
    paths.runtimeDirectory = linkedAncestor / "nested/prismdrake";

    const auto runtime = SessionRuntime::prepare(paths);

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, ErrorCode::invalid_environment);
    EXPECT_TRUE(std::filesystem::is_symlink(linkedAncestor));
    EXPECT_TRUE(std::filesystem::is_directory(prismdrake));
}

TEST(SessionRuntimeTest, RejectsLexicalParentTraversal) {
    RuntimeFixture fixture;
    auto paths = fixture.paths();
    paths.runtimeDirectory = fixture.prismdrake() / ".." / "prismdrake";

    const auto runtime = SessionRuntime::prepare(paths);

    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error().code, ErrorCode::invalid_environment);
    EXPECT_TRUE(std::filesystem::is_directory(fixture.prismdrake()));
}

TEST(SessionRuntimeTest, RefusesBroadCleanupWhenAnUnexpectedEntryExists) {
    RuntimeFixture fixture;
    const auto sibling = fixture.prismdrake() / "sibling";
    writeFile(sibling, "preserve");
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    const auto instance = runtime.value().instanceDirectory();
    const auto unexpected = instance / "unexpected-private-content";
    writeFile(unexpected, "preserve");

    const auto cleanup = runtime.value().cleanup();

    ASSERT_FALSE(cleanup);
    EXPECT_TRUE(std::filesystem::is_regular_file(unexpected));
    EXPECT_TRUE(std::filesystem::is_regular_file(sibling));
    EXPECT_TRUE(std::filesystem::is_directory(instance));
}

TEST(SessionRuntimeTest, RefusesToRemoveAReplacedMarkerOrItsSymlinkTarget) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value().markReady());
    const auto external = fixture.root() / "external-private-content";
    writeFile(external, "preserve");
    ASSERT_TRUE(std::filesystem::remove(runtime.value().readyMarkerPath()));
    ASSERT_EQ(::symlink(external.c_str(), runtime.value().readyMarkerPath().c_str()), 0);

    const auto cleanup = runtime.value().cleanup();

    ASSERT_FALSE(cleanup);
    EXPECT_TRUE(std::filesystem::is_symlink(runtime.value().readyMarkerPath()));
    EXPECT_TRUE(std::filesystem::is_regular_file(external));
}

TEST(SessionRuntimeTest, RepeatedMarkerCallDetectsADeletedMarker) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value().markReady());
    ASSERT_TRUE(std::filesystem::remove(runtime.value().readyMarkerPath()));

    const auto repeated = runtime.value().markReady();

    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error().code, ErrorCode::permission_denied);
    EXPECT_FALSE(std::filesystem::exists(runtime.value().readyMarkerPath()));
}

TEST(SessionRuntimeTest, RefusesToRemoveAReplacedInstancePath) {
    RuntimeFixture fixture;
    auto runtime = SessionRuntime::prepare(fixture.paths());
    ASSERT_TRUE(runtime);
    const auto instance = runtime.value().instanceDirectory();
    const auto external = fixture.root() / "external-directory";
    std::filesystem::create_directory(external);
    writeFile(external / "private-content", "preserve");
    ASSERT_TRUE(std::filesystem::remove(instance));
    ASSERT_EQ(::symlink(external.c_str(), instance.c_str()), 0);

    const auto cleanup = runtime.value().cleanup();

    ASSERT_FALSE(cleanup);
    EXPECT_TRUE(std::filesystem::is_symlink(instance));
    EXPECT_TRUE(std::filesystem::is_regular_file(external / "private-content"));
}

} // namespace
} // namespace prismdrake::session
