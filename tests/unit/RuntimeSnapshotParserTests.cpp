#include "RuntimeSnapshotParser.hpp"

#include "RuntimeSnapshot.hpp"
#include "SettingsEngine.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace prismdrake::shell::settings {
namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint counter{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("prismdrake-snapshot-parser-test-" + std::to_string(counter.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string readFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("unable to read snapshot parser fixture");
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("unable to write snapshot parser fixture");
    }
}

[[nodiscard]] std::shared_ptr<const prismdrake::settings::SettingsSnapshot> realSnapshot() {
    TemporaryDirectory temporary;
    const auto source = std::filesystem::path(PRISMDRAKE_SOURCE_DIR);
    const config::ConfigurationLocations locations{temporary.path() / "config/config.toml",
                                                   temporary.path() /
                                                       "state/last-known-valid-config.toml",
                                                   source / "data/defaults/config.toml"};
    writeFile(locations.user, readFile(source / "examples/config/lustre.toml"));
    auto engine = prismdrake::settings::SettingsEngine::start(
        {locations, source / "themes", {}, {{true, true}, false}});
    if (!engine) {
        throw std::runtime_error(engine.error().message);
    }
    return engine.value()->current();
}

TEST(RuntimeSnapshotParserTest, ReconstructsCompleteCanonicalTypedSnapshot) {
    const auto source = realSnapshot();

    auto parsed = parseRuntimeSnapshot(source->generation.value(), source->serializedJson);

    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value()->snapshotSchemaVersion, source->snapshotSchemaVersion);
    EXPECT_EQ(parsed.value()->generation, source->generation);
    EXPECT_EQ(parsed.value()->candidate, source->candidate);
    EXPECT_EQ(parsed.value()->serializedJson, source->serializedJson);
}

TEST(RuntimeSnapshotParserTest, RejectsGenerationMismatchAndNoncanonicalBytes) {
    const auto source = realSnapshot();

    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value() + 1U, source->serializedJson));
    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), source->serializedJson + "\n"));
}

TEST(RuntimeSnapshotParserTest, RejectsMalformedDuplicateDeepAndOversizedInputs) {
    const auto source = realSnapshot();
    auto duplicate = source->serializedJson;
    duplicate.insert(1U, "\"schema_version\":1,");
    std::string deep = "[";
    deep.append(30U, '[');
    deep.append(30U, ']');
    deep.push_back(']');
    std::string oversized(prismdrake::settings::maximumRuntimeSnapshotBytes + 1U, 'x');

    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), "{"));
    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), duplicate));
    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), deep));
    const auto rejected = parseRuntimeSnapshot(source->generation.value(), oversized);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, foundation::ErrorCode::too_large);
}

TEST(RuntimeSnapshotParserTest, RejectsCanonicalShapedInvalidAndUnknownFields) {
    const auto source = realSnapshot();
    auto invalidAccent = nlohmann::ordered_json::parse(source->serializedJson);
    invalidAccent["settings"]["appearance"]["accent"] = "#GGGGGG";
    auto unknownMetric = nlohmann::ordered_json::parse(source->serializedJson);
    unknownMetric["theme"]["semantic"]["targets"]["unexpected_px"] = 24.0;

    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), invalidAccent.dump()));
    EXPECT_FALSE(parseRuntimeSnapshot(source->generation.value(), unknownMetric.dump()));
}

} // namespace
} // namespace prismdrake::shell::settings
