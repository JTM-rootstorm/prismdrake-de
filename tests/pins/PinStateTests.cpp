#include "PinState.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace prismdrake::shell::panel {
namespace {

class TemporaryState final {
  public:
    TemporaryState() {
        std::string pattern = "/tmp/prismdrake-pin-state-tests.XXXXXX";
        char *created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error{"Could not create the pin-state fixture."};
        }
        root_ = created;
    }

    ~TemporaryState() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] std::filesystem::path file() const { return root_ / "pins.json"; }

    void write(std::string contents, mode_t mode = 0600U) const {
        std::ofstream output(file(), std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error{"Could not write the pin-state fixture."};
        }
        output << contents;
        output.close();
        if (::chmod(file().c_str(), mode) != 0) {
            throw std::runtime_error{"Could not set pin-state fixture permissions."};
        }
    }

    [[nodiscard]] std::string read() const {
        std::ifstream input(file(), std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] launcher::DesktopFileId pin(std::string value) {
    auto result = launcher::deriveDesktopFileId(value);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

TEST(PinStateTest, MissingStateStartsEmptyAndAtomicWriteSurvivesRestart) {
    TemporaryState state;
    auto opened = PinStateStore::open(state.file());
    ASSERT_TRUE(opened);
    auto store = std::move(opened).value();
    EXPECT_EQ(store.current()->generation, 1U);
    EXPECT_EQ(store.current()->loadStatus, PinStateLoadStatus::missing);
    EXPECT_TRUE(store.current()->pins.empty());

    auto published = store.replace(
        {pin("org.prismdrake.Control.desktop"), pin("org.prismdrake.Terminal.desktop")});
    ASSERT_TRUE(published);
    EXPECT_EQ(published.value()->generation, 2U);
    ASSERT_EQ(published.value()->pins.size(), 2U);

    struct stat metadata = {};
    ASSERT_EQ(::lstat(state.file().c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777U, 0600U);

    auto restarted = PinStateStore::open(state.file());
    ASSERT_TRUE(restarted);
    EXPECT_EQ(restarted.value().current()->loadStatus, PinStateLoadStatus::loaded);
    ASSERT_EQ(restarted.value().current()->pins.size(), 2U);
    EXPECT_EQ(restarted.value().current()->pins[0].value(), "org.prismdrake.Control.desktop");
    EXPECT_EQ(restarted.value().current()->pins[1].value(), "org.prismdrake.Terminal.desktop");
}

TEST(PinStateTest, DuplicateMutationPreservesPublishedAndOnDiskState) {
    TemporaryState state;
    auto opened = PinStateStore::open(state.file());
    ASSERT_TRUE(opened);
    auto store = std::move(opened).value();
    ASSERT_TRUE(store.replace({pin("first.desktop")}));
    const auto before = state.read();
    const auto generation = store.current()->generation;

    const auto duplicate = pin("duplicate.desktop");
    auto rejected = store.replace({duplicate, duplicate});

    EXPECT_FALSE(rejected);
    EXPECT_EQ(store.current()->generation, generation);
    EXPECT_EQ(state.read(), before);
}

TEST(PinStateTest, MalformedOversizedAndInsecureFilesAreRetainedWithEmptyRecovery) {
    TemporaryState malformed;
    malformed.write("{ private malformed content\n");
    const auto malformedBefore = malformed.read();
    auto malformedStore = PinStateStore::open(malformed.file());
    ASSERT_TRUE(malformedStore);
    EXPECT_EQ(malformedStore.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
    EXPECT_TRUE(malformedStore.value().current()->pins.empty());
    EXPECT_EQ(malformed.read(), malformedBefore);

    TemporaryState oversized;
    oversized.write(std::string(maximumPinStateBytes + 1U, 'x'));
    auto oversizedStore = PinStateStore::open(oversized.file());
    ASSERT_TRUE(oversizedStore);
    EXPECT_EQ(oversizedStore.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
    EXPECT_EQ(std::filesystem::file_size(oversized.file()), maximumPinStateBytes + 1U);

    TemporaryState insecure;
    insecure.write("{\"schema_version\":1,\"pins\":[]}", 0644U);
    auto insecureStore = PinStateStore::open(insecure.file());
    ASSERT_TRUE(insecureStore);
    EXPECT_EQ(insecureStore.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
    struct stat metadata = {};
    ASSERT_EQ(::lstat(insecure.file().c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777U, 0644U);
    auto insecureOwner = std::move(insecureStore).value();
    EXPECT_FALSE(insecureOwner.replace({pin("replacement.desktop")}));
    ASSERT_EQ(::lstat(insecure.file().c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777U, 0644U);
}

TEST(PinStateTest, RejectsUnknownKeysVersionsPathsAndDuplicateDiskPins) {
    const std::vector<std::string> invalidDocuments{
        R"({"schema_version":2,"pins":[]})", R"({"schema_version":1,"pins":[],"generation":9})",
        R"({"schema_version":1,"pins":["../unsafe.desktop"]})",
        R"({"schema_version":1,"pins":["same.desktop","same.desktop"]})",
        R"({"schema_version":1,"pins":["not-a-desktop-id"]})"};
    for (const auto &document : invalidDocuments) {
        TemporaryState state;
        state.write(document);
        auto store = PinStateStore::open(state.file());
        ASSERT_TRUE(store);
        EXPECT_EQ(store.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
        EXPECT_TRUE(store.value().current()->pins.empty());
        EXPECT_EQ(state.read(), document);
    }
}

TEST(PinStateTest, RejectsDeepAndHighNodeJsonBeforeDomPublication) {
    std::string highNode = R"({"schema_version":1,"pins":[)";
    for (std::size_t index = 0U; index <= maximumPinCount; ++index) {
        if (index != 0U) {
            highNode.push_back(',');
        }
        highNode.append("\"entry-").append(std::to_string(index)).append(".desktop\"");
    }
    highNode.append("]}");
    const std::vector<std::string> hostileDocuments{
        R"({"schema_version":1,"pins":[[[[[["deep.desktop"]]]]]]})", highNode};
    for (const auto &document : hostileDocuments) {
        TemporaryState state;
        state.write(document);
        auto store = PinStateStore::open(state.file());
        ASSERT_TRUE(store);
        EXPECT_EQ(store.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
        EXPECT_TRUE(store.value().current()->pins.empty());
        EXPECT_EQ(state.read(), document);
    }
}

TEST(PinStateTest, RejectsSymlinkWithoutReadingOrReplacingItsTarget) {
    TemporaryState target;
    target.write("{\"schema_version\":1,\"pins\":[\"target.desktop\"]}");
    TemporaryState link;
    std::error_code linkError;
    std::filesystem::create_symlink(target.file(), link.file(), linkError);
    ASSERT_FALSE(linkError);

    auto store = PinStateStore::open(link.file());

    ASSERT_TRUE(store);
    EXPECT_EQ(store.value().current()->loadStatus, PinStateLoadStatus::invalidRetained);
    EXPECT_TRUE(store.value().current()->pins.empty());
    EXPECT_TRUE(std::filesystem::is_symlink(link.file()));
    EXPECT_NE(target.read().find("target.desktop"), std::string::npos);
}

} // namespace
} // namespace prismdrake::shell::panel
