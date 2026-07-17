#include "SdBusApi.hpp"
#include "SettingsReadiness.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using prismdrake::ipc::sdbus::Bus;
using prismdrake::ipc::sdbus::Message;
using prismdrake::ipc::sdbus::Slot;

constexpr char serviceName[] = "org.prismdrake.Settings1";
constexpr char objectPath[] = "/org/prismdrake/Settings1";
constexpr char settingsInterface[] = "org.prismdrake.Settings1";
constexpr char snapshotInterface[] = "org.prismdrake.SettingsSnapshot1";
constexpr std::size_t maximumCandidateBytes = std::size_t{1024} * 1024U;

template <typename T> [[nodiscard]] T requireOptional(std::optional<T> value) {
    if (!value) {
        throw std::logic_error("required test optional is empty");
    }
    return std::move(value).value();
}

struct ErrorCapture final {
    sd_bus_error value{};

    ~ErrorCapture() { sd_bus_error_free(&value); }
    ErrorCapture(const ErrorCapture &) = delete;
    ErrorCapture &operator=(const ErrorCapture &) = delete;
    ErrorCapture() = default;
};

struct ProfileReply final {
    std::string profile;
    std::uint64_t generation;
};

struct SignalCapture final {
    std::size_t count = 0;
    std::uint64_t generation = 0;
    std::string profile;
    bool restartRequired = true;
    std::string domains;
    std::string warnings;
};

[[nodiscard]] std::string readStringArray(sd_bus_message *message) {
    std::string values;
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") < 0) {
        return values;
    }
    const char *value = nullptr;
    while (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, static_cast<void *>(&value)) >
           0) {
        if (value != nullptr) {
            values += value;
            values.push_back(' ');
        }
    }
    (void)sd_bus_message_exit_container(message);
    return values;
}

int captureGenerationSignal(sd_bus_message *message, void *userData, sd_bus_error *) {
    auto &capture = *static_cast<SignalCapture *>(userData);
    const char *profile = nullptr;
    int restartRequired = 0;
    if (sd_bus_message_read(message, "t", &capture.generation) < 0) {
        return -EINVAL;
    }
    capture.domains = readStringArray(message);
    if (sd_bus_message_read(message, "sb", &profile, &restartRequired) < 0) {
        return -EINVAL;
    }
    capture.warnings = readStringArray(message);
    capture.profile = profile == nullptr ? std::string{} : profile;
    capture.restartRequired = restartRequired != 0;
    ++capture.count;
    return 0;
}

class SettingsServiceFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        const char *executable = std::getenv("PRISMDRAKE_SETTINGSD_EXECUTABLE");
        ASSERT_NE(executable, nullptr);
        executable_ = executable;

        std::array<char, 48> pathTemplate{};
        constexpr std::string_view prefix = "/tmp/prismdrake-settingsd-test.XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        ASSERT_NE(created, nullptr);
        root_ = created;

        createPrivateDirectory(root_ / "runtime");
        createPrivateDirectory(root_ / "runtime" / "prismdrake");
        createPrivateDirectory(root_ / "config");
        createPrivateDirectory(root_ / "state");
        createPrivateDirectory(root_ / "data");
        createPrivateDirectory(root_ / "cache");
        createPrivateDirectory(root_ / "home");

        setEnvironment("HOME", root_ / "home");
        setEnvironment("XDG_RUNTIME_DIR", root_ / "runtime");
        setEnvironment("XDG_CONFIG_HOME", root_ / "config");
        setEnvironment("XDG_STATE_HOME", root_ / "state");
        setEnvironment("XDG_DATA_HOME", root_ / "data");
        setEnvironment("XDG_CACHE_HOME", root_ / "cache");

        ASSERT_GE(sd_bus_open_user(bus_.put()), 0);
        startService();
        ASSERT_TRUE(waitForService());
    }

    void TearDown() override {
        bus_.reset();
        if (child_ > 0) {
            (void)::kill(child_, SIGTERM);
            int status = 0;
            (void)::waitpid(child_, &status, 0);
            child_ = -1;
        }
        if (!root_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }
    }

    void createPrivateDirectory(const std::filesystem::path &path) {
        ASSERT_TRUE(std::filesystem::create_directory(path));
        ASSERT_EQ(::chmod(path.c_str(), 0700), 0);
    }

    static void setEnvironment(const char *name, const std::filesystem::path &value) {
        ASSERT_EQ(::setenv(name, value.c_str(), 1), 0);
    }

    void startService() {
        child_ = ::fork();
        ASSERT_GE(child_, 0);
        if (child_ != 0) {
            return;
        }

        const auto stderrPath = root_ / "settingsd.stderr";
        const int stderrFile =
            ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (stderrFile < 0 || ::dup2(stderrFile, STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)::close(stderrFile);
        ::execl(executable_.c_str(), executable_.c_str(), "--foreground", nullptr);
        _exit(127);
    }

    [[nodiscard]] bool waitForService() {
        constexpr auto deadline = std::chrono::seconds{5};
        const auto end = std::chrono::steady_clock::now() + deadline;
        while (std::chrono::steady_clock::now() < end) {
            auto profile = currentProfile();
            if (profile.has_value()) {
                return true;
            }
            int status = 0;
            if (::waitpid(child_, &status, WNOHANG) == child_) {
                child_ = -1;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    [[nodiscard]] std::optional<ProfileReply> currentProfile() {
        ErrorCapture error;
        Message reply;
        if (sd_bus_call_method(bus_.get(), serviceName, objectPath, settingsInterface,
                               "GetCurrentProfile", &error.value, reply.put(), "") < 0) {
            return std::nullopt;
        }
        const char *profile = nullptr;
        std::uint64_t generation = 0;
        if (sd_bus_message_read(reply.get(), "st", &profile, &generation) < 0 ||
            profile == nullptr) {
            return std::nullopt;
        }
        return ProfileReply{profile, generation};
    }

    [[nodiscard]] std::uint64_t requestProfile(std::string_view profile) {
        ErrorCapture error;
        Message reply;
        const std::string profileString{profile};
        if (sd_bus_call_method(bus_.get(), serviceName, objectPath, settingsInterface,
                               "RequestProfileChange", &error.value, reply.put(), "s",
                               profileString.c_str()) < 0) {
            ADD_FAILURE() << "profile request failed: "
                          << (error.value.name == nullptr ? "unknown" : error.value.name);
            return 0;
        }
        std::uint64_t generation = 0;
        EXPECT_GE(sd_bus_message_read(reply.get(), "t", &generation), 0);
        return generation;
    }

    [[nodiscard]] std::uint64_t reload() {
        ErrorCapture error;
        Message reply;
        if (sd_bus_call_method(bus_.get(), serviceName, objectPath, settingsInterface, "Reload",
                               &error.value, reply.put(), "") < 0) {
            ADD_FAILURE() << "reload failed: "
                          << (error.value.name == nullptr ? "unknown" : error.value.name);
            return 0;
        }
        std::uint64_t generation = 0;
        EXPECT_GE(sd_bus_message_read(reply.get(), "t", &generation), 0);
        return generation;
    }

    [[nodiscard]] std::pair<std::uint64_t, std::string>
    currentSnapshot(std::uint32_t version = 1U) {
        ErrorCapture error;
        Message reply;
        if (sd_bus_call_method(bus_.get(), serviceName, objectPath, snapshotInterface,
                               "GetCurrentSnapshot", &error.value, reply.put(), "u", version) < 0) {
            ADD_FAILURE() << "snapshot request failed: "
                          << (error.value.name == nullptr ? "unknown" : error.value.name);
            return {};
        }
        std::uint64_t generation = 0;
        const void *bytes = nullptr;
        std::size_t size = 0;
        EXPECT_GE(sd_bus_message_read(reply.get(), "t", &generation), 0);
        EXPECT_GE(sd_bus_message_read_array(reply.get(), SD_BUS_TYPE_BYTE, &bytes, &size), 0);
        return {generation, std::string{static_cast<const char *>(bytes), size}};
    }

    [[nodiscard]] std::string callStringErrorName(const char *interface, const char *member,
                                                  const char *value) {
        ErrorCapture error;
        Message reply;
        const int result = sd_bus_call_method(bus_.get(), serviceName, objectPath, interface,
                                              member, &error.value, reply.put(), "s", value);
        EXPECT_LT(result, 0);
        return error.value.name == nullptr ? std::string{} : std::string{error.value.name};
    }

    [[nodiscard]] std::string callVersionErrorName(const char *interface, const char *member,
                                                   std::uint32_t value) {
        ErrorCapture error;
        Message reply;
        const int result = sd_bus_call_method(bus_.get(), serviceName, objectPath, interface,
                                              member, &error.value, reply.put(), "u", value);
        EXPECT_LT(result, 0);
        return error.value.name == nullptr ? std::string{} : std::string{error.value.name};
    }

    [[nodiscard]] std::string callNoArgumentErrorName(const char *interface, const char *member) {
        ErrorCapture error;
        Message reply;
        const int result = sd_bus_call_method(bus_.get(), serviceName, objectPath, interface,
                                              member, &error.value, reply.put(), "");
        EXPECT_LT(result, 0);
        return error.value.name == nullptr ? std::string{} : std::string{error.value.name};
    }

    [[nodiscard]] static std::string defaultConfiguration() {
        std::ifstream stream(std::filesystem::path{PRISMDRAKE_SOURCE_DIR} /
                             "data/defaults/config.toml");
        EXPECT_TRUE(stream);
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    void writeUserConfiguration(std::string_view document) {
        const auto directory = root_ / "config/prismdrake";
        std::filesystem::create_directories(directory);
        std::ofstream stream(directory / "config.toml", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream);
        stream.write(document.data(), static_cast<std::streamsize>(document.size()));
        ASSERT_TRUE(stream);
    }

    [[nodiscard]] std::pair<bool, std::string> validateCandidate(std::string_view candidate,
                                                                 std::string *errorName = nullptr) {
        Message call;
        EXPECT_GE(sd_bus_message_new_method_call(bus_.get(), call.put(), serviceName, objectPath,
                                                 settingsInterface, "ValidateCandidate"),
                  0);
        // append_array consumes exactly candidate.size() bytes; no C-string terminator is needed.
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
        EXPECT_GE(sd_bus_message_append_array(call.get(), SD_BUS_TYPE_BYTE, candidate.data(),
                                              candidate.size()),
                  0);
        ErrorCapture error;
        Message reply;
        const int callResult =
            sd_bus_call(bus_.get(), call.get(), 6'000'000U, &error.value, reply.put());
        if (callResult < 0) {
            if (errorName != nullptr) {
                *errorName = error.value.name == nullptr ? std::string{} : error.value.name;
            }
            return {false, {}};
        }

        int valid = 0;
        EXPECT_GE(sd_bus_message_read(reply.get(), "b", &valid), 0);
        EXPECT_GE(sd_bus_message_enter_container(reply.get(), SD_BUS_TYPE_ARRAY, "(sssss)"), 0);
        std::string diagnostics;
        while (sd_bus_message_enter_container(reply.get(), SD_BUS_TYPE_STRUCT, "sssss") > 0) {
            std::array<const char *, 5> fields{};
            EXPECT_GE(sd_bus_message_read(reply.get(), "sssss", &fields[0], &fields[1], &fields[2],
                                          &fields[3], &fields[4]),
                      0);
            for (const char *field : fields) {
                if (field != nullptr) {
                    diagnostics += field;
                }
            }
            EXPECT_GE(sd_bus_message_exit_container(reply.get()), 0);
        }
        EXPECT_GE(sd_bus_message_exit_container(reply.get()), 0);
        return {valid != 0, std::move(diagnostics)};
    }

    void pumpBusUntil(const std::function<bool()> &condition, std::chrono::milliseconds timeout) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!condition() && std::chrono::steady_clock::now() < end) {
            int processed = 0;
            do {
                processed = sd_bus_process(bus_.get(), nullptr);
            } while (processed > 0);
            ASSERT_GE(processed, 0);
            if (!condition()) {
                ASSERT_GE(sd_bus_wait(bus_.get(), 10'000U), 0);
            }
        }
    }

    std::filesystem::path root_;
    std::string executable_;
    pid_t child_ = -1;
    Bus bus_;
};

TEST_F(SettingsServiceFixture, PublishesCompleteInitialSnapshotAndNoOpReload) {
    const auto profile = currentProfile();
    ASSERT_TRUE(profile.has_value());
    const auto profileValue = requireOptional(profile);
    EXPECT_EQ(profileValue.profile, "lustre");
    EXPECT_EQ(profileValue.generation, 1U);

    const auto [generation, snapshot] = currentSnapshot();
    EXPECT_EQ(generation, 1U);
    EXPECT_LE(snapshot.size(), maximumCandidateBytes);
    EXPECT_NE(snapshot.find("\"generation\":1"), std::string::npos);
    EXPECT_NE(snapshot.find("\"profile_id\":\"lustre\""), std::string::npos);
    EXPECT_EQ(snapshot.find(root_.string()), std::string::npos);
    EXPECT_EQ(reload(), 1U);
}

TEST_F(SettingsServiceFixture, SatisfiesTheBoundedSessionReadinessProbe) {
    const auto readiness = prismdrake::session::probeSettingsReadiness(std::chrono::seconds{2});

    ASSERT_TRUE(readiness);
    EXPECT_EQ(readiness.value().generation, 1U);
    EXPECT_GT(readiness.value().snapshotBytes, 0U);
    EXPECT_LE(readiness.value().snapshotBytes,
              prismdrake::session::maximumSettingsReadinessSnapshotBytes);
}

TEST_F(SettingsServiceFixture, ProfileRequestsAreAtomicAndTyped) {
    const char *invalid = "invalid-profile";
    EXPECT_EQ(callStringErrorName(settingsInterface, "RequestProfileChange", invalid),
              "org.prismdrake.Settings1.Error.InvalidProfile");
    EXPECT_EQ(requestProfile("lustre"), 1U);
    EXPECT_EQ(requestProfile("forge"), 2U);

    const auto profile = currentProfile();
    ASSERT_TRUE(profile.has_value());
    const auto profileValue = requireOptional(profile);
    EXPECT_EQ(profileValue.profile, "forge");
    EXPECT_EQ(profileValue.generation, 2U);

    const std::uint32_t unsupported = 2U;
    EXPECT_EQ(callVersionErrorName(snapshotInterface, "GetCurrentSnapshot", unsupported),
              "org.prismdrake.Settings1.Error.UnsupportedSnapshotVersion");
}

TEST_F(SettingsServiceFixture, CandidateValidationIsBoundedAndRedacted) {
    constexpr std::string_view sentinel = "prismdrake-secret-sentinel";
    const auto valid = validateCandidate(defaultConfiguration());
    EXPECT_TRUE(valid.first);
    EXPECT_TRUE(valid.second.empty());

    const auto invalid =
        validateCandidate("schema_version = 1\nunknown = \"" + std::string{sentinel} + "\"\n");
    EXPECT_FALSE(invalid.first);
    EXPECT_FALSE(invalid.second.empty());
    EXPECT_EQ(invalid.second.find(sentinel), std::string::npos);
    EXPECT_EQ(invalid.second.find(root_.string()), std::string::npos);

    const std::string exactlyBounded(maximumCandidateBytes, ' ');
    std::string exactError;
    (void)validateCandidate(exactlyBounded, &exactError);
    EXPECT_TRUE(exactError.empty());

    const std::string oversized(maximumCandidateBytes + 1U, ' ');
    std::string oversizeError;
    (void)validateCandidate(oversized, &oversizeError);
    EXPECT_EQ(oversizeError, "org.prismdrake.Settings1.Error.TooLarge");

    const auto profile = currentProfile();
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(requireOptional(profile).generation, 1U);
}

TEST_F(SettingsServiceFixture, ReloadPublishesValidInputAndRetainsItAfterFailure) {
    auto forgeConfiguration = defaultConfiguration();
    const auto profile = forgeConfiguration.find("profile = \"lustre\"");
    ASSERT_NE(profile, std::string::npos);
    forgeConfiguration.replace(profile, std::string_view{"profile = \"lustre\""}.size(),
                               "profile = \"forge\"");
    writeUserConfiguration(forgeConfiguration);

    EXPECT_EQ(reload(), 2U);
    auto current = currentProfile();
    ASSERT_TRUE(current);
    EXPECT_EQ(requireOptional(current).profile, "forge");

    writeUserConfiguration("schema_version = 1\nprivate_secret = \"reload-sentinel\"\n");
    EXPECT_EQ(callNoArgumentErrorName(settingsInterface, "Reload"),
              "org.prismdrake.Settings1.Error.ValidationFailed");
    current = currentProfile();
    ASSERT_TRUE(current);
    const auto retained = requireOptional(current);
    EXPECT_EQ(retained.profile, "forge");
    EXPECT_EQ(retained.generation, 2U);
}

TEST_F(SettingsServiceFixture, EmitsOneStructuredSignalOnlyForARealPublication) {
    SignalCapture capture;
    Slot match;
    ASSERT_GE(
        sd_bus_add_match(bus_.get(), match.put(),
                         "type='signal',path='/org/prismdrake/Settings1',"
                         "interface='org.prismdrake.Settings1',member='SettingsGenerationChanged'",
                         captureGenerationSignal, &capture),
        0);

    EXPECT_EQ(reload(), 1U);
    pumpBusUntil([&capture] { return capture.count != 0U; }, std::chrono::milliseconds{50});
    EXPECT_EQ(capture.count, 0U);

    EXPECT_EQ(requestProfile("forge"), 2U);
    pumpBusUntil([&capture] { return capture.count == 1U; }, std::chrono::seconds{1});
    ASSERT_EQ(capture.count, 1U);
    EXPECT_EQ(capture.generation, 2U);
    EXPECT_EQ(capture.profile, "forge");
    EXPECT_FALSE(capture.restartRequired);
    EXPECT_NE(capture.domains.find("profile"), std::string::npos);
    EXPECT_NE(capture.domains.find("theme"), std::string::npos);

    EXPECT_EQ(requestProfile("forge"), 2U);
    pumpBusUntil([&capture] { return capture.count > 1U; }, std::chrono::milliseconds{50});
    EXPECT_EQ(capture.count, 1U);
}

} // namespace
