#include "BoundedFile.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::foundation {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::string pathTemplate = "/tmp/prismdrake-bounded-file-tests.XXXXXX";
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

void writeFixture(const std::filesystem::path &path, std::string_view payload) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ASSERT_TRUE(output.good());
}

TEST(BoundedFileTest, DistinguishesMissingFile) {
    TemporaryDirectory temporary;
    const auto result = readBoundedFile(temporary.path() / "missing", 16);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::not_found);
}

TEST(BoundedFileTest, DistinguishesPermissionDenied) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can bypass ordinary file permission checks";
    }

    TemporaryDirectory temporary;
    const auto path = temporary.path() / "private";
    writeFixture(path, "private");
    ASSERT_EQ(::chmod(path.c_str(), 0000), 0);

    const auto result = readBoundedFile(path, 16);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::permission_denied);
}

TEST(BoundedFileTest, RejectsNonRegularInputWithoutReadingIt) {
    TemporaryDirectory temporary;

    const auto result = readBoundedFile(temporary.path(), 16);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::unsupported);
}

TEST(BoundedFileTest, RejectsZeroLimit) {
    TemporaryDirectory temporary;
    const auto result = readBoundedFile(temporary.path() / "unused", 0);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::invalid_argument);
}

TEST(BoundedFileTest, RejectsOversizedFile) {
    TemporaryDirectory temporary;
    writeFixture(temporary.path() / "oversized", "12345");

    const auto result = readBoundedFile(temporary.path() / "oversized", 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::too_large);
}

TEST(BoundedFileTest, AcceptsFileAtExactLimit) {
    TemporaryDirectory temporary;
    writeFixture(temporary.path() / "exact", "12345");

    const auto result = readBoundedFile(temporary.path() / "exact", 5);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "12345");
}

TEST(BoundedFileTest, AcceptsEmptyFile) {
    TemporaryDirectory temporary;
    writeFixture(temporary.path() / "empty", "");

    const auto result = readBoundedFile(temporary.path() / "empty", 1);

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().empty());
}

TEST(BoundedFileTest, PreservesBinaryPayload) {
    TemporaryDirectory temporary;
    const std::string payload{"a\0b\0c", 5};
    writeFixture(temporary.path() / "binary", payload);

    const auto result = readBoundedFile(temporary.path() / "binary", payload.size());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), payload);
}

} // namespace
} // namespace prismdrake::foundation
