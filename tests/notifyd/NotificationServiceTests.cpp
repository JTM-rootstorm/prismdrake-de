#include "NotificationService.hpp"
#include "SdBusApi.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using prismdrake::ipc::sdbus::Bus;
using prismdrake::ipc::sdbus::Message;
using prismdrake::ipc::sdbus::Slot;

constexpr char standardService[] = "org.freedesktop.Notifications";
constexpr char standardPath[] = "/org/freedesktop/Notifications";
constexpr char standardInterface[] = "org.freedesktop.Notifications";
constexpr char centerService[] = "org.prismdrake.NotificationCenter1";
constexpr char centerPath[] = "/org/prismdrake/NotificationCenter1";
constexpr char centerInterface[] = "org.prismdrake.NotificationCenter1";

TEST(NotificationServiceTransportTest, FailedPostCommitSignalLatchesEpochDegradation) {
    prismdrake::notifyd::PostCommitTransportGuard guard;
    guard.observe(true);
    EXPECT_FALSE(guard.degraded());
    guard.observe(false);
    EXPECT_TRUE(guard.degraded());
    guard.observe(true);
    EXPECT_TRUE(guard.degraded());
}

struct ErrorCapture final {
    sd_bus_error value{};
    ~ErrorCapture() { sd_bus_error_free(&value); }
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::array<char, 56> path{};
        constexpr std::string_view pattern = "/tmp/prismdrake-notifyd-bus.XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        char *created = ::mkdtemp(path.data());
        if (created == nullptr) {
            throw std::runtime_error{"temporary directory creation failed"};
        }
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

struct NotifyReply final {
    int result{0};
    std::uint32_t id{0U};
    std::string errorName;
};

struct SignalCapture final {
    std::size_t actions{0U};
    std::size_t closes{0U};
    std::uint32_t id{0U};
    std::uint32_t reason{0U};
    std::string action;
};

int captureAction(sd_bus_message *message, void *userData, sd_bus_error *) {
    auto &capture = *static_cast<SignalCapture *>(userData);
    const char *action = nullptr;
    if (sd_bus_message_read(message, "us", &capture.id, &action) < 0 || action == nullptr) {
        return -EINVAL;
    }
    capture.action = action;
    ++capture.actions;
    return 0;
}

int captureClose(sd_bus_message *message, void *userData, sd_bus_error *) {
    auto &capture = *static_cast<SignalCapture *>(userData);
    if (sd_bus_message_read(message, "uu", &capture.id, &capture.reason) < 0) {
        return -EINVAL;
    }
    ++capture.closes;
    return 0;
}

[[nodiscard]] bool processUntil(sd_bus *bus, const std::function<bool()> &predicate,
                                std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int result = 0;
        do {
            result = sd_bus_process(bus, nullptr);
        } while (result > 0);
        if (predicate()) {
            return true;
        }
        pollfd descriptor{sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0};
        (void)::poll(&descriptor, 1U, 20);
    }
    return predicate();
}

[[nodiscard]] NotifyReply notify(sd_bus *bus, std::uint32_t replacesId, std::string_view summary,
                                 const std::vector<std::string> &actionValues = {},
                                 std::optional<std::uint8_t> urgency = std::nullopt,
                                 std::int32_t timeout = -1, std::size_t ignoredImageBytes = 0U,
                                 bool duplicateUrgency = false) {
    Message call;
    Message reply;
    NotifyReply outcome;
    if (sd_bus_message_new_method_call(bus, call.put(), standardService, standardPath,
                                       standardInterface, "Notify") < 0) {
        outcome.result = -EINVAL;
        return outcome;
    }
    const std::string summaryValue{summary};
    if (sd_bus_message_append(call.get(), "susss", "Fixture", replacesId, "", summaryValue.c_str(),
                              "Literal body") < 0 ||
        sd_bus_message_open_container(call.get(), SD_BUS_TYPE_ARRAY, "s") < 0) {
        outcome.result = -EINVAL;
        return outcome;
    }
    for (const auto &value : actionValues) {
        if (sd_bus_message_append_basic(call.get(), SD_BUS_TYPE_STRING, value.c_str()) < 0) {
            outcome.result = -EINVAL;
            return outcome;
        }
    }
    if (sd_bus_message_close_container(call.get()) < 0 ||
        sd_bus_message_open_container(call.get(), SD_BUS_TYPE_ARRAY, "{sv}") < 0) {
        outcome.result = -EINVAL;
        return outcome;
    }
    if (urgency) {
        const unsigned entries = duplicateUrgency ? 2U : 1U;
        for (unsigned entry = 0U; entry < entries; ++entry) {
            if (sd_bus_message_open_container(call.get(), SD_BUS_TYPE_DICT_ENTRY, "sv") < 0 ||
                sd_bus_message_append(call.get(), "s", "urgency") < 0 ||
                sd_bus_message_open_container(call.get(), SD_BUS_TYPE_VARIANT, "y") < 0 ||
                sd_bus_message_append_basic(call.get(), SD_BUS_TYPE_BYTE, &*urgency) < 0 ||
                sd_bus_message_close_container(call.get()) < 0 ||
                sd_bus_message_close_container(call.get()) < 0) {
                outcome.result = -EINVAL;
                return outcome;
            }
        }
    }
    std::vector<std::uint8_t> ignoredImage(ignoredImageBytes, 0x5AU);
    if (!ignoredImage.empty()) {
        if (sd_bus_message_open_container(call.get(), SD_BUS_TYPE_DICT_ENTRY, "sv") < 0 ||
            sd_bus_message_append(call.get(), "s", "image-data") < 0 ||
            sd_bus_message_open_container(call.get(), SD_BUS_TYPE_VARIANT, "ay") < 0 ||
            sd_bus_message_append_array(call.get(), SD_BUS_TYPE_BYTE, ignoredImage.data(),
                                        ignoredImage.size()) < 0 ||
            sd_bus_message_close_container(call.get()) < 0 ||
            sd_bus_message_close_container(call.get()) < 0) {
            outcome.result = -EINVAL;
            return outcome;
        }
    }
    if (sd_bus_message_close_container(call.get()) < 0 ||
        sd_bus_message_append(call.get(), "i", timeout) < 0) {
        outcome.result = -EINVAL;
        return outcome;
    }
    ErrorCapture error;
    outcome.result = sd_bus_call(bus, call.get(), 2000000U, &error.value, reply.put());
    if (outcome.result < 0) {
        outcome.errorName = error.value.name == nullptr ? std::string{} : error.value.name;
        return outcome;
    }
    if (sd_bus_message_read(reply.get(), "u", &outcome.id) < 0) {
        outcome.result = -EINVAL;
    }
    return outcome;
}

[[nodiscard]] std::optional<nlohmann::json> currentState(sd_bus *bus) {
    ErrorCapture error;
    Message reply;
    if (sd_bus_call_method(bus, centerService, centerPath, centerInterface, "GetCurrentState",
                           &error.value, reply.put(), "u", 1U) < 0) {
        return std::nullopt;
    }
    std::uint64_t generation = 0U;
    const void *bytes = nullptr;
    std::size_t size = 0U;
    if (sd_bus_message_read(reply.get(), "t", &generation) < 0 || generation == 0U ||
        sd_bus_message_read_array(reply.get(), SD_BUS_TYPE_BYTE, &bytes, &size) < 0 ||
        bytes == nullptr || size == 0U) {
        return std::nullopt;
    }
    auto parsed = nlohmann::json::parse(static_cast<const char *>(bytes),
                                        static_cast<const char *>(bytes) + size, nullptr, false);
    if (parsed.is_discarded() || parsed.value("generation", 0U) != generation) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] pid_t startNotifyd(const std::filesystem::path &executable,
                                 const std::filesystem::path &root) {
    const pid_t child = ::fork();
    if (child != 0) {
        return child;
    }
    const auto stderrPath = root / "notifyd.stderr";
    const int stderrFile =
        ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (stderrFile < 0 || ::dup2(stderrFile, STDERR_FILENO) < 0) {
        _exit(126);
    }
    (void)::close(stderrFile);
    (void)::setenv("HOME", (root / "home").c_str(), 1);
    (void)::setenv("XDG_RUNTIME_DIR", (root / "runtime").c_str(), 1);
    (void)::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    (void)::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    ::execl(executable.c_str(), executable.c_str(), "--foreground", nullptr);
    _exit(127);
}

void createPrivateDirectories(const std::filesystem::path &root) {
    for (const auto &name : {"home", "runtime", "state", "config"}) {
        const auto path = root / name;
        ASSERT_TRUE(std::filesystem::create_directory(path));
        ASSERT_EQ(::chmod(path.c_str(), 0700), 0);
    }
}

class NotificationServiceFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        const char *executable = std::getenv("PRISMDRAKE_NOTIFYD_EXECUTABLE");
        ASSERT_NE(executable, nullptr);
        executable_ = executable;
        createPrivateDirectories(temporary_.path());
        ASSERT_GE(sd_bus_open_user(bus_.put()), 0);
        child_ = startNotifyd(executable_, temporary_.path());
        ASSERT_GT(child_, 0);
        ASSERT_TRUE(
            processUntil(bus_.get(), [this] { return currentState(bus_.get()).has_value(); }, 5s));
    }

    void TearDown() override {
        second_bus_.reset();
        bus_.reset();
        if (child_ > 0) {
            (void)::kill(child_, SIGTERM);
            int status = 0;
            (void)::waitpid(child_, &status, 0);
            child_ = -1;
        }
    }

    [[nodiscard]] Bus &secondBus() {
        if (!second_bus_) {
            EXPECT_GE(sd_bus_open_user(second_bus_.put()), 0);
        }
        return second_bus_;
    }

    TemporaryDirectory temporary_;
    std::filesystem::path executable_;
    Bus bus_;
    Bus second_bus_;
    pid_t child_{-1};
};

TEST_F(NotificationServiceFixture, PublishesConservativeVersionedReadinessContract) {
    ErrorCapture error;
    Message reply;
    ASSERT_GE(sd_bus_call_method(bus_.get(), standardService, standardPath, standardInterface,
                                 "GetServerInformation", &error.value, reply.put(), ""),
              0);
    const char *name = nullptr;
    const char *vendor = nullptr;
    const char *version = nullptr;
    const char *specification = nullptr;
    ASSERT_GE(sd_bus_message_read(reply.get(), "ssss", &name, &vendor, &version, &specification),
              0);
    EXPECT_STREQ(name, "Prismdrake");
    EXPECT_STREQ(vendor, "Prismdrake");
    EXPECT_STREQ(specification, "1.3");

    reply.reset();
    ASSERT_GE(sd_bus_call_method(bus_.get(), standardService, standardPath, standardInterface,
                                 "GetCapabilities", &error.value, reply.put(), ""),
              0);
    ASSERT_GT(sd_bus_message_enter_container(reply.get(), SD_BUS_TYPE_ARRAY, "s"), 0);
    std::vector<std::string> capabilities;
    const char *capability = nullptr;
    while (sd_bus_message_read_basic(reply.get(), SD_BUS_TYPE_STRING, &capability) > 0) {
        capabilities.emplace_back(capability);
    }
    EXPECT_EQ(capabilities, (std::vector<std::string>{"actions", "body", "persistence"}));

    auto state = currentState(bus_.get());
    ASSERT_TRUE(state);
    EXPECT_EQ(state->at("generation"), 1U);
    EXPECT_TRUE(state->at("records").empty());
}

TEST_F(NotificationServiceFixture, NativeStateMutationsUseTypedGenerationAndStaleGuards) {
    const auto created = notify(bus_.get(), 0U, "Native", {"open", "Open"});
    ASSERT_GE(created.result, 0);
    auto state = currentState(bus_.get());
    ASSERT_TRUE(state);
    const auto &record = state->at("records").front();
    const auto recordId = record.at("record_id").get<std::uint64_t>();
    const auto contentGeneration = record.at("content_generation").get<std::uint64_t>();
    const auto priorGeneration = state->at("generation").get<std::uint64_t>();

    ErrorCapture dndError;
    Message dndReply;
    ASSERT_GE(sd_bus_call_method(bus_.get(), centerService, centerPath, centerInterface,
                                 "SetDoNotDisturb", &dndError.value, dndReply.put(), "b", 1),
              0);
    std::uint64_t dndGeneration = 0U;
    ASSERT_GE(sd_bus_message_read(dndReply.get(), "t", &dndGeneration), 0);
    EXPECT_GT(dndGeneration, priorGeneration);
    state = currentState(bus_.get());
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->at("effective_do_not_disturb"));
    EXPECT_FALSE(state->at("records").front().at("transient"));

    ErrorCapture staleError;
    Message staleReply;
    EXPECT_LT(sd_bus_call_method(bus_.get(), centerService, centerPath, centerInterface, "Dismiss",
                                 &staleError.value, staleReply.put(), "tt", recordId,
                                 contentGeneration + 1U),
              0);
    ASSERT_NE(staleError.value.name, nullptr);
    EXPECT_STREQ(staleError.value.name, "org.prismdrake.NotificationCenter1.Error.StaleRecord");
    const auto afterStale = currentState(bus_.get());
    ASSERT_TRUE(afterStale);
    EXPECT_EQ(afterStale->at("generation"), dndGeneration);

    ErrorCapture schemaError;
    Message schemaReply;
    EXPECT_LT(sd_bus_call_method(bus_.get(), centerService, centerPath, centerInterface,
                                 "GetCurrentState", &schemaError.value, schemaReply.put(), "u", 2U),
              0);
    ASSERT_NE(schemaError.value.name, nullptr);
    EXPECT_STREQ(schemaError.value.name,
                 "org.prismdrake.NotificationCenter1.Error.UnsupportedSchemaVersion");
}

TEST_F(NotificationServiceFixture, ReplacementTransfersActionAndCloseSignalsToNewSender) {
    auto &replacementBus = secondBus();
    SignalCapture capture;
    Slot actionSlot;
    Slot closeSlot;
    ASSERT_GE(sd_bus_match_signal(replacementBus.get(), actionSlot.put(), standardService,
                                  standardPath, standardInterface, "ActionInvoked", captureAction,
                                  &capture),
              0);
    ASSERT_GE(sd_bus_match_signal(replacementBus.get(), closeSlot.put(), standardService,
                                  standardPath, standardInterface, "NotificationClosed",
                                  captureClose, &capture),
              0);

    const auto first = notify(bus_.get(), 0U, "First", {"open", "Open"});
    ASSERT_GE(first.result, 0);
    const auto replacement =
        notify(replacementBus.get(), first.id, "Replaced", {"respond", "Respond"});
    ASSERT_GE(replacement.result, 0);
    EXPECT_EQ(replacement.id, first.id);
    auto state = currentState(bus_.get());
    ASSERT_TRUE(state);
    ASSERT_EQ(state->at("records").size(), 1U);
    const auto &record = state->at("records").front();
    const auto recordId = record.at("record_id").get<std::uint64_t>();
    const auto contentGeneration = record.at("content_generation").get<std::uint64_t>();
    EXPECT_EQ(contentGeneration, 2U);

    ErrorCapture actionError;
    Message actionReply;
    ASSERT_GE(sd_bus_call_method(bus_.get(), centerService, centerPath, centerInterface,
                                 "InvokeAction", &actionError.value, actionReply.put(), "tts",
                                 recordId, contentGeneration, "respond"),
              0);
    ASSERT_TRUE(processUntil(replacementBus.get(), [&capture] { return capture.actions == 1U; }));
    EXPECT_EQ(capture.id, first.id);
    EXPECT_EQ(capture.action, "respond");

    ErrorCapture closeError;
    Message closeReply;
    ASSERT_GE(sd_bus_call_method(bus_.get(), standardService, standardPath, standardInterface,
                                 "CloseNotification", &closeError.value, closeReply.put(), "u",
                                 first.id),
              0);
    ASSERT_TRUE(processUntil(replacementBus.get(), [&capture] { return capture.closes == 1U; }));
    EXPECT_EQ(capture.id, first.id);
    EXPECT_EQ(capture.reason, 3U);
    state = currentState(bus_.get());
    ASSERT_TRUE(state);
    EXPECT_FALSE(state->at("records").front().contains("notification_id"));
    EXPECT_FALSE(state->at("records").front().at("actions").front().at("enabled"));
}

TEST_F(NotificationServiceFixture, UnknownReplacementReturnsAndUsesTheRequestedLiveId) {
    constexpr std::uint32_t requested = 900U;
    const auto created = notify(bus_.get(), requested, "Requested identifier");
    ASSERT_GE(created.result, 0);
    EXPECT_EQ(created.id, requested);

    auto state = currentState(bus_.get());
    ASSERT_TRUE(state);
    ASSERT_EQ(state->at("records").size(), 1U);
    EXPECT_EQ(state->at("records").front().at("notification_id"), requested);

    const auto allocated = notify(bus_.get(), 0U, "Allocated identifier");
    ASSERT_GE(allocated.result, 0);
    EXPECT_NE(allocated.id, requested);
    EXPECT_NE(allocated.id, 0U);
}

TEST_F(NotificationServiceFixture, SenderLossDisablesActionsAndTimeoutClosesMonotonically) {
    Bus sender;
    ASSERT_GE(sd_bus_open_user(sender.put()), 0);
    const auto created = notify(sender.get(), 0U, "Sender", {"open", "Open"});
    ASSERT_GE(created.result, 0);
    sender.reset();

    ASSERT_TRUE(processUntil(bus_.get(), [this] {
        const auto state = currentState(bus_.get());
        return state && !state->at("records").empty() &&
               !state->at("records").front().at("actions").front().at("enabled").get<bool>();
    }));

    SignalCapture capture;
    Slot closeSlot;
    ASSERT_GE(sd_bus_match_signal(bus_.get(), closeSlot.put(), standardService, standardPath,
                                  standardInterface, "NotificationClosed", captureClose, &capture),
              0);
    const auto expiring = notify(bus_.get(), 0U, "Timeout", {}, std::nullopt, 50);
    ASSERT_GE(expiring.result, 0);
    ASSERT_TRUE(processUntil(bus_.get(), [&capture] { return capture.closes == 1U; }));
    EXPECT_EQ(capture.id, expiring.id);
    EXPECT_EQ(capture.reason, 1U);
}

TEST_F(NotificationServiceFixture, RejectsMalformedAndOversizedStandardPayloadsWithoutMutation) {
    const auto before = currentState(bus_.get());
    ASSERT_TRUE(before);
    const auto oddActions = notify(bus_.get(), 0U, "Odd", {"key"});
    EXPECT_LT(oddActions.result, 0);
    EXPECT_EQ(oddActions.errorName, "org.freedesktop.DBus.Error.InvalidArgs");

    const auto oversizedSummary = notify(bus_.get(), 0U, std::string(1025U, 'x'));
    EXPECT_LT(oversizedSummary.result, 0);
    EXPECT_EQ(oversizedSummary.errorName, "org.freedesktop.DBus.Error.LimitsExceeded");

    const auto oversizedHint =
        notify(bus_.get(), 0U, "Image", {}, std::nullopt, -1, 1024U * 1024U + 1U);
    EXPECT_LT(oversizedHint.result, 0);
    EXPECT_EQ(oversizedHint.errorName, "org.freedesktop.DBus.Error.LimitsExceeded");

    const auto duplicateHint = notify(bus_.get(), 0U, "Duplicate", {}, 1U, -1, 0U, true);
    EXPECT_LT(duplicateHint.result, 0);
    EXPECT_EQ(duplicateHint.errorName, "org.freedesktop.DBus.Error.InvalidArgs");

    const auto after = currentState(bus_.get());
    ASSERT_TRUE(after);
    EXPECT_EQ(after->at("generation"), before->at("generation"));
    EXPECT_EQ(after->at("records"), before->at("records"));
}

TEST(NotificationServiceConflictTest, ExistingStandardOwnerExitsCleanlyWithoutRetryLoop) {
    const char *executable = std::getenv("PRISMDRAKE_NOTIFYD_EXECUTABLE");
    ASSERT_NE(executable, nullptr);
    Bus owner;
    ASSERT_GE(sd_bus_open_user(owner.put()), 0);
    ASSERT_GE(sd_bus_request_name(owner.get(), standardService, 0), 0);
    TemporaryDirectory temporary;
    createPrivateDirectories(temporary.path());
    const pid_t child = startNotifyd(executable, temporary.path());
    ASSERT_GT(child, 0);

    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if (!exited) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    EXPECT_TRUE(exited);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 3);
    (void)sd_bus_release_name(owner.get(), standardService);
}

} // namespace
