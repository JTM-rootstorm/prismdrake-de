#include "NotificationCenterClient.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace prismdrake::shell::notificationcenter {
namespace {

constexpr char serviceName[] = "org.prismdrake.NotificationCenter1";
constexpr char objectPath[] = "/org/prismdrake/NotificationCenter1";
constexpr char interfaceName[] = "org.prismdrake.NotificationCenter1";

struct Response final {
    std::uint64_t generation;
    std::string bytes;
};

[[nodiscard]] Response response(std::uint64_t generation, bool doNotDisturb) {
    const auto enabled = doNotDisturb ? "true" : "false";
    return {generation,
            "{\"schema_version\":1,\"generation\":" + std::to_string(generation) +
                ",\"history_mode\":\"persistent\",\"effective_do_not_disturb\":" + enabled +
                ",\"runtime_do_not_disturb_override\":" + enabled +
                ",\"records\":[{\"record_id\":7,\"content_generation\":3,"
                "\"application_name\":\"Fixture\",\"desktop_file_id\":\"fixture.desktop\","
                "\"summary\":\"Literal <b>summary</b>\",\"body\":\"Body\","
                "\"urgency\":\"normal\",\"received_at_unix_ms\":1234,"
                "\"dismissible\":true,\"transient\":true,"
                "\"actions\":[{\"action_id\":\"open\",\"label\":\"Open\","
                "\"enabled\":true}],\"notification_id\":9}]}"};
}

struct FakeServiceOptions final {
    bool noReplyMutation{false};
};

class FakeNotificationService final {
  public:
    explicit FakeNotificationService(std::vector<Response> responses,
                                     FakeServiceOptions options = {})
        : responses_(std::move(responses)), options_(options) {
        if (responses_.empty()) {
            throw std::invalid_argument("fake notification service requires a response");
        }
        std::promise<bool> ready;
        auto result = ready.get_future();
        thread_ =
            std::thread([this, signal = std::move(ready)]() mutable { run(std::move(signal)); });
        if (!result.get()) {
            running_.store(false);
            thread_.join();
            throw std::runtime_error("unable to start fake notification service");
        }
    }

    ~FakeNotificationService() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FakeNotificationService(const FakeNotificationService &) = delete;
    FakeNotificationService &operator=(const FakeNotificationService &) = delete;

    void advanceAndSignal() noexcept { advance_.store(true); }
    [[nodiscard]] std::uint32_t mutationCount() const noexcept { return mutation_count_.load(); }

  private:
    [[nodiscard]] static const sd_bus_vtable *vtable() {
        static const sd_bus_vtable table[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("GetCurrentState", "u", "tay", getCurrentState,
                          SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("SetDoNotDisturb", "b", "t", setDoNotDisturb, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("ClearHistory", "", "t", clearHistory, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Dismiss", "tt", "t", dismiss, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("InvokeAction", "tts", "t", invokeAction, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END};
        return table;
    }

    static int getCurrentState(sd_bus_message *message, void *userdata,
                               sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeNotificationService *>(userdata);
        std::uint32_t version = 0U;
        if (sd_bus_message_read(message, "u", &version) <= 0 ||
            version != notificationCenterStateSchemaVersion ||
            sd_bus_message_at_end(message, 1) <= 0) {
            return sd_bus_error_set_const(
                error, "org.prismdrake.NotificationCenter1.Error.UnsupportedSchemaVersion",
                "Unsupported state schema version.");
        }
        const auto &current = self.responses_.at(self.index_);
        ipc::sdbus::Message reply;
        if (sd_bus_message_new_method_return(message, reply.put()) < 0 ||
            sd_bus_message_append(reply.get(), "t", current.generation) < 0 ||
            sd_bus_message_append_array(reply.get(), SD_BUS_TYPE_BYTE, current.bytes.data(),
                                        current.bytes.size()) < 0) {
            return -ENOMEM;
        }
        return sd_bus_send(self.bus_, reply.get(), nullptr);
    }

    static int setDoNotDisturb(sd_bus_message *message, void *userdata,
                               sd_bus_error *error) noexcept {
        auto &self = *static_cast<FakeNotificationService *>(userdata);
        int enabled = 0;
        if (sd_bus_message_read(message, "b", &enabled) <= 0 ||
            sd_bus_message_at_end(message, 1) <= 0) {
            return -EINVAL;
        }
        ++self.mutation_count_;
        if (enabled != 0 && self.index_ + 1U < self.responses_.size()) {
            ++self.index_;
        }
        if (self.options_.noReplyMutation) {
            return sd_bus_error_set_const(error, "org.freedesktop.DBus.Error.NoReply",
                                          "Mutation result unavailable.");
        }
        return sd_bus_reply_method_return(message, "t", self.responses_.at(self.index_).generation);
    }

    static int clearHistory(sd_bus_message *message, void *userdata, sd_bus_error *) noexcept {
        auto &self = *static_cast<FakeNotificationService *>(userdata);
        ++self.mutation_count_;
        return sd_bus_reply_method_return(message, "t", self.responses_.at(self.index_).generation);
    }

    static int dismiss(sd_bus_message *message, void *userdata, sd_bus_error *) noexcept {
        auto &self = *static_cast<FakeNotificationService *>(userdata);
        std::uint64_t recordId = 0U;
        std::uint64_t contentGeneration = 0U;
        if (sd_bus_message_read(message, "tt", &recordId, &contentGeneration) <= 0 ||
            recordId != 7U || contentGeneration != 3U) {
            return -EINVAL;
        }
        ++self.mutation_count_;
        return sd_bus_reply_method_return(message, "t", self.responses_.at(self.index_).generation);
    }

    static int invokeAction(sd_bus_message *message, void *userdata, sd_bus_error *) noexcept {
        auto &self = *static_cast<FakeNotificationService *>(userdata);
        std::uint64_t recordId = 0U;
        std::uint64_t contentGeneration = 0U;
        const char *actionId = nullptr;
        if (sd_bus_message_read(message, "tts", &recordId, &contentGeneration, &actionId) <= 0 ||
            recordId != 7U || contentGeneration != 3U || actionId == nullptr ||
            std::string_view{actionId} != "open") {
            return -EINVAL;
        }
        ++self.mutation_count_;
        return sd_bus_reply_method_return(message, "t", self.responses_.at(self.index_).generation);
    }

    void emitStateChanged() noexcept {
        ipc::sdbus::Message signal;
        if (sd_bus_message_new_signal(bus_, signal.put(), objectPath, interfaceName,
                                      "StateChanged") < 0 ||
            sd_bus_message_append(signal.get(), "t", responses_.at(index_).generation) < 0 ||
            sd_bus_message_open_container(signal.get(), SD_BUS_TYPE_ARRAY, "s") < 0 ||
            sd_bus_message_append(signal.get(), "s", "do_not_disturb") < 0 ||
            sd_bus_message_close_container(signal.get()) < 0) {
            return;
        }
        (void)sd_bus_send(bus_, signal.get(), nullptr);
    }

    void run(std::promise<bool> ready) noexcept {
        ipc::sdbus::Bus bus;
        sd_bus *opened = nullptr;
        if (sd_bus_open_user(&opened) < 0 || opened == nullptr) {
            ready.set_value(false);
            return;
        }
        bus.reset(opened);
        bus_ = bus.get();
        ipc::sdbus::Slot object;
        if (sd_bus_add_object_vtable(bus.get(), object.put(), objectPath, interfaceName, vtable(),
                                     this) < 0 ||
            sd_bus_request_name(bus.get(), serviceName, 0U) <= 0) {
            bus_ = nullptr;
            ready.set_value(false);
            return;
        }
        ready.set_value(true);

        while (running_.load()) {
            if (advance_.exchange(false) && index_ + 1U < responses_.size()) {
                ++index_;
                emitStateChanged();
            }
            int processed = 0;
            do {
                processed = sd_bus_process(bus.get(), nullptr);
            } while (processed > 0);
            if (processed < 0) {
                break;
            }
            pollfd descriptor{sd_bus_get_fd(bus.get()),
                              static_cast<short>(sd_bus_get_events(bus.get())), 0};
            (void)::poll(&descriptor, 1U, 10);
        }
        bus_ = nullptr;
    }

    const std::vector<Response> responses_;
    const FakeServiceOptions options_;
    std::atomic_bool running_{true};
    std::atomic_bool advance_{false};
    std::atomic_uint32_t mutation_count_{0U};
    std::thread thread_;
    sd_bus *bus_{nullptr};
    std::size_t index_{0U};
};

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate predicate,
                             std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

TEST(NotificationCenterClientTest, FetchesAndRefetchesOnlyCompleteState) {
    FakeNotificationService service({response(1U, false), response(2U, true)});
    NotificationCenterClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(
        waitUntil([&]() { return client.state() == NotificationCenterClient::State::ready; }));
    ASSERT_TRUE(client.currentSnapshot());
    EXPECT_EQ(client.currentSnapshot()->generation, 1U);

    service.advanceAndSignal();
    ASSERT_TRUE(waitUntil(
        [&]() { return client.currentSnapshot() && client.currentSnapshot()->generation == 2U; }));
    EXPECT_TRUE(client.currentSnapshot()->effectiveDoNotDisturb);
}

TEST(NotificationCenterClientTest, ClearsPrivateStateAcrossOwnerLossAndReacquisition) {
    auto service =
        std::make_unique<FakeNotificationService>(std::vector<Response>{response(1U, false)});
    NotificationCenterClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    service.reset();
    ASSERT_TRUE(waitUntil([&]() {
        return client.state() == NotificationCenterClient::State::unavailable &&
               client.currentSnapshot() == nullptr;
    }));

    service = std::make_unique<FakeNotificationService>(std::vector<Response>{response(3U, true)});
    ASSERT_TRUE(waitUntil(
        [&]() { return client.currentSnapshot() && client.currentSnapshot()->generation == 3U; }));
}

TEST(NotificationCenterClientTest, EnforcesOneMutationAndRefetchesAfterSuccess) {
    FakeNotificationService service({response(1U, false), response(2U, true)});
    NotificationCenterClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    ASSERT_TRUE(client.setDoNotDisturb(true));
    auto second = client.clearHistory();
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, foundation::ErrorCode::invalid_argument);
    ASSERT_TRUE(waitUntil([&]() {
        return client.currentSnapshot() && client.currentSnapshot()->generation == 2U &&
               client.state() == NotificationCenterClient::State::ready;
    }));
    EXPECT_EQ(service.mutationCount(), 1U);
}

TEST(NotificationCenterClientTest, RefetchesAmbiguousNoReplyWithoutRetryingMutation) {
    FakeNotificationService service({response(1U, false), response(2U, true)}, {true});
    NotificationCenterClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    ASSERT_TRUE(client.setDoNotDisturb(true));
    ASSERT_TRUE(waitUntil([&]() {
        return client.currentSnapshot() && client.currentSnapshot()->generation == 2U &&
               client.activeMutation() == NotificationCenterClient::Mutation::none;
    }));
    EXPECT_EQ(service.mutationCount(), 1U);
    EXPECT_TRUE(client.currentSnapshot()->effectiveDoNotDisturb);
}

TEST(NotificationCenterClientTest, RejectsStaleDeadAndUnknownCommandsLocally) {
    FakeNotificationService service({response(1U, false)});
    NotificationCenterClient client;
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitUntil([&]() { return client.currentSnapshot() != nullptr; }));

    EXPECT_FALSE(client.dismiss({7U, 4U}));
    EXPECT_FALSE(client.invokeAction({7U, 3U}, QStringLiteral("missing")));
    EXPECT_EQ(service.mutationCount(), 0U);
    ASSERT_TRUE(client.invokeAction({7U, 3U}, QStringLiteral("open")));
    ASSERT_TRUE(waitUntil(
        [&]() { return client.activeMutation() == NotificationCenterClient::Mutation::none; }));
    EXPECT_EQ(service.mutationCount(), 1U);
}

} // namespace
} // namespace prismdrake::shell::notificationcenter

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
