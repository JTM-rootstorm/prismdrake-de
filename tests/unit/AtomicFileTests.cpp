#include "AtomicFile.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::foundation {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::string pathTemplate = "/tmp/prismdrake-atomic-file-tests.XXXXXX";
        char *created = ::mkdtemp(pathTemplate.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class ScopedFileSizeLimit final {
  public:
    explicit ScopedFileSizeLimit(rlim_t bytes) {
        if (::getrlimit(RLIMIT_FSIZE, &previousLimit_) != 0) {
            throw std::runtime_error("getrlimit failed");
        }

        struct sigaction ignoredAction{};
        ignoredAction.sa_handler = SIG_IGN;
        if (::sigemptyset(&ignoredAction.sa_mask) != 0 ||
            ::sigaction(SIGXFSZ, &ignoredAction, &previousAction_) != 0) {
            throw std::runtime_error("sigaction failed");
        }

        struct rlimit limited = previousLimit_;
        limited.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            ::sigaction(SIGXFSZ, &previousAction_, nullptr);
            throw std::runtime_error("setrlimit failed");
        }
        active_ = true;
    }

    ~ScopedFileSizeLimit() {
        if (active_) {
            ::setrlimit(RLIMIT_FSIZE, &previousLimit_);
            ::sigaction(SIGXFSZ, &previousAction_, nullptr);
        }
    }

    ScopedFileSizeLimit(const ScopedFileSizeLimit &) = delete;
    ScopedFileSizeLimit &operator=(const ScopedFileSizeLimit &) = delete;

  private:
    struct rlimit previousLimit_{};
    struct sigaction previousAction_{};
    bool active_ = false;
};

void writeFixture(const std::filesystem::path &path, std::string_view payload) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_TRUE(output.good());
}

[[nodiscard]] std::string readFixture(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] mode_t permissionsOf(const std::filesystem::path &path) {
    struct stat metadata{};
    EXPECT_EQ(::stat(path.c_str(), &metadata), 0);
    return metadata.st_mode & 0777;
}

void expectNoTemporaryFiles(const std::filesystem::path &directory) {
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        EXPECT_EQ(entry.path().filename().string().find(".prismdrake.tmp."), std::string::npos);
    }
}

TEST(AtomicFileTest, RejectsSymbolicLinkDestination) {
    TemporaryDirectory temporary;
    const auto target = temporary.path() / "target";
    const auto link = temporary.path() / "link";
    writeFixture(target, "original");
    ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);

    const auto result = writeFileAtomically(link, "replacement");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(readFixture(target), "original");
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    expectNoTemporaryFiles(temporary.path());
}

TEST(AtomicFileTest, RejectsSymbolicLinkInParentChain) {
    TemporaryDirectory temporary;
    const auto actualParent = temporary.path() / "actual";
    const auto linkedParent = temporary.path() / "linked";
    ASSERT_TRUE(std::filesystem::create_directory(actualParent));
    writeFixture(actualParent / "settings", "original");
    ASSERT_EQ(::symlink(actualParent.c_str(), linkedParent.c_str()), 0);

    const auto result = writeFileAtomically(linkedParent / "settings", "replacement");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
    EXPECT_EQ(readFixture(actualParent / "settings"), "original");
    expectNoTemporaryFiles(actualParent);
}

TEST(AtomicFileTest, RejectsDestinationOwnedByAnotherUser) {
    if (::geteuid() != 0) {
        GTEST_SKIP() << "changing fixture ownership requires root";
    }

    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "foreign-owned";
    writeFixture(destination, "original");
    ASSERT_EQ(::chown(destination.c_str(), 65534, 65534), 0);

    const auto result = writeFileAtomically(destination, "replacement");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::permission_denied);
    EXPECT_EQ(readFixture(destination), "original");
}

TEST(AtomicFileTest, ReplacesFileAndPreservesPermissions) {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "settings";
    writeFixture(destination, "old");
    ASSERT_EQ(::chmod(destination.c_str(), 0640), 0);

    const auto result = writeFileAtomically(destination, "new settings");

    ASSERT_TRUE(result);
    EXPECT_EQ(readFixture(destination), "new settings");
    EXPECT_EQ(permissionsOf(destination), 0640);
    expectNoTemporaryFiles(temporary.path());
}

TEST(AtomicFileTest, CreatesFileWithRequestedPermissionsAndBinaryPayload) {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "new-settings";
    const std::string payload{"a\0b\0c", 5};
    AtomicWriteOptions options;
    options.createPermissions = std::filesystem::perms::owner_read |
                                std::filesystem::perms::owner_write |
                                std::filesystem::perms::group_read;

    const auto result = writeFileAtomically(destination, payload, options);

    ASSERT_TRUE(result);
    EXPECT_EQ(readFixture(destination), payload);
    EXPECT_EQ(permissionsOf(destination), 0640);
    expectNoTemporaryFiles(temporary.path());
}

TEST(AtomicFileTest, DeterministicTemporaryNameFailurePreservesExistingFile) {
    TemporaryDirectory temporary;
    const long nameLimit = ::pathconf(temporary.path().c_str(), _PC_NAME_MAX);
    ASSERT_GT(nameLimit, 32);
    const std::string longName(static_cast<std::size_t>(nameLimit - 1), 'x');
    const auto destination = temporary.path() / longName;
    writeFixture(destination, "last known valid");

    const auto result = writeFileAtomically(destination, "replacement");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::io_error);
    EXPECT_EQ(readFixture(destination), "last known valid");
    expectNoTemporaryFiles(temporary.path());
}

TEST(AtomicFileTest, InterruptedPayloadWritePreservesExistingFile) {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "settings";
    writeFixture(destination, "last known valid");
    ScopedFileSizeLimit limit(1);

    const auto result = writeFileAtomically(destination, std::string(4096, 'x'));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::io_error);
    EXPECT_EQ(readFixture(destination), "last known valid");
    expectNoTemporaryFiles(temporary.path());
}

} // namespace
} // namespace prismdrake::foundation
